#include "Instantiator.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <tuple>
#include <utility>

using namespace llvm;

namespace c_autotmpl {

namespace {

constexpr StringLiteral MarkerPrefix("/* c-autotmpl: ");
constexpr StringLiteral MarkerSuffix(" */");

uint64_t stampFor(StringRef Path) {
  sys::fs::file_status Status;
  if (sys::fs::status(Path, Status))
    return 0;
  uint64_t Time = static_cast<uint64_t>(
      Status.getLastModificationTime().time_since_epoch().count());
  return (Time << 1) ^ static_cast<uint64_t>(Status.getSize());
}

bool isSpace(char C) {
  return C == ' ' || C == '\t' || C == '\r' || C == '\n' || C == '\f' ||
         C == '\v';
}

bool parseDefine(StringRef Line, StringRef Param, StringRef &Type) {
  Line = Line.trim();
  if (!Line.starts_with("#define"))
    return false;
  StringRef Rest = Line.drop_front(strlen("#define")).ltrim();
  if (!Rest.starts_with(Param))
    return false;
  Rest = Rest.drop_front(Param.size());
  if (!Rest.empty() && !isSpace(Rest.front()))
    return false;
  Type = Rest.trim();
  return !Type.empty();
}

bool lineIncludes(StringRef Line, StringRef Include) {
  Line = Line.trim();
  if (!Line.starts_with("#include"))
    return false;
  StringRef Rest = Line.drop_front(strlen("#include")).trim();
  if (Rest.size() < 2 || Rest.front() != '"' || Rest.back() != '"')
    return false;
  return Rest.drop_front().drop_back() == Include;
}

bool lineUndefines(StringRef Line, StringRef Param) {
  Line = Line.trim();
  if (!Line.starts_with("#undef"))
    return false;
  StringRef Rest = Line.drop_front(strlen("#undef")).trim();
  return Rest == Param;
}

struct LineInfo {
  StringRef Text;
  size_t Begin;
  size_t End; // Includes the trailing newline, when present.
};

SmallVector<LineInfo, 64> splitLines(StringRef Content) {
  SmallVector<LineInfo, 64> Lines;
  size_t Pos = 0;
  while (Pos <= Content.size()) {
    size_t Newline = Content.find('\n', Pos);
    size_t TextEnd =
        Newline == StringRef::npos ? Content.size() : Newline;
    Lines.push_back({Content.slice(Pos, TextEnd), Pos,
                     Newline == StringRef::npos ? Content.size() : Newline + 1});
    if (Newline == StringRef::npos)
      break;
    Pos = Newline + 1;
  }
  return Lines;
}

/// Finds one instantiation block at Lines[Index], marked or hand-written, and
/// reports its byte range. Trailing blank lines are included so removal does
/// not leave extra whitespace behind.
bool findBlockAt(ArrayRef<LineInfo> Lines, size_t Index,
                 const TemplateDef &Template, StringRef &Type, size_t &Begin,
                 size_t &End, bool &Marked) {
  if (Index >= Lines.size())
    return false;

  StringRef Param =
      Template.Params.empty() ? StringRef("T") : Template.Params.front();
  StringRef First = Lines[Index].Text.trim();

  if (First.starts_with(MarkerPrefix) && First.ends_with(MarkerSuffix)) {
    StringRef Inner =
        First.drop_front(MarkerPrefix.size()).drop_back(MarkerSuffix.size());
    size_t Open = Inner.find('(');
    size_t Close = Inner.rfind(')');
    if (Open == StringRef::npos || Close == StringRef::npos || Close < Open)
      return false;
    if (Inner.take_front(Open).trim() != Template.Name)
      return false;
    Type = Inner.slice(Open + 1, Close).trim();
    Marked = true;
  } else {
    if (!parseDefine(First, Param, Type))
      return false;
    Marked = false;
  }
  if (Type.empty())
    return false;

  bool SawInclude = false;
  bool SawConditional = false;
  int Depth = 0;
  size_t UndefIndex = StringRef::npos;
  size_t LastLine = StringRef::npos;
  size_t Last = std::min(Index + 18, Lines.size());
  for (size_t I = Index + 1; I < Last; ++I) {
    StringRef Line = Lines[I].Text.trim();
    if (Line.starts_with("#if ") || Line.starts_with("#ifdef") ||
        Line.starts_with("#ifndef")) {
      ++Depth;
      SawConditional = true;
    } else if (Line.starts_with("#endif")) {
      if (Depth > 0)
        --Depth;
      if (SawConditional && Depth == 0 && UndefIndex != StringRef::npos) {
        LastLine = I;
        break;
      }
    }
    if (lineIncludes(Line, Template.IncludeFromGenDir))
      SawInclude = true;
    if (lineUndefines(Line, Param)) {
      UndefIndex = I;
      // Unguarded blocks end at their #undef. Guarded blocks keep scanning to
      // the matching #endif so nested presence/guard pairs are handled.
      if (!SawConditional)
        break;
    }
  }
  if (!SawInclude || UndefIndex == StringRef::npos)
    return false;

  if (LastLine == StringRef::npos)
    LastLine = UndefIndex;
  if (LastLine + 1 < Lines.size() && Lines[LastLine + 1].Text.trim().empty())
    ++LastLine;

  Begin = Lines[Index].Begin;
  if (LastLine + 1 < Lines.size())
    End = Lines[LastLine + 1].Begin;
  else
    End = Lines[LastLine].End;
  return true;
}

/// Recognizes hand-written instantiation blocks in a generated header. These
/// have no c-autotmpl marker, so without this check the plugin would append a
/// second block for the same type and cause duplicate definitions.
bool hasManualInstantiation(StringRef Content, const TemplateDef &Template,
                            StringRef TypeSpelling) {
  auto Lines = splitLines(Content);
  size_t SkipUntil = 0;
  for (size_t I = 0; I < Lines.size(); ++I) {
    if (Lines[I].Begin < SkipUntil)
      continue;
    StringRef Type;
    size_t Begin, End;
    bool Marked = false;
    if (!findBlockAt(Lines, I, Template, Type, Begin, End, Marked))
      continue;
    // A generated block contains `#define T ...` lines of its own. Once its
    // marker is seen, skip the whole block so its body is not mistaken for a
    // separate hand-written instantiation.
    SkipUntil = End;
    if (!Marked && Type == TypeSpelling)
      return true;
  }
  return false;
}

} // namespace

std::string instantiationMarker(StringRef TemplateName, StringRef TypeSpelling) {
  std::string Out;
  Out.reserve(MarkerPrefix.size() + TemplateName.size() + TypeSpelling.size() +
              MarkerSuffix.size() + 4);
  Out.append(MarkerPrefix.data(), MarkerPrefix.size());
  Out += TemplateName;
  Out += '(';
  Out += TypeSpelling;
  Out += ')';
  Out.append(MarkerSuffix.data(), MarkerSuffix.size());
  return Out;
}

/// A per-instantiation macro makes duplicate definitions harmless even when
/// the same nested type is emitted in more than one generated header.
std::string presenceMacro(StringRef TemplateName, StringRef TypeSpelling) {
  std::string Name = TemplateName.str() + "_" + TypeSpelling.str();
  uint32_t Hash = 2166136261u;
  for (unsigned char C : Name) {
    Hash ^= C;
    Hash *= 16777619u;
  }

  std::string Out = "C_AUTOTMPL_";
  for (unsigned char C : Name) {
    if (C >= 'a' && C <= 'z')
      Out += static_cast<char>(C - 'a' + 'A');
    else if ((C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9'))
      Out += static_cast<char>(C);
    else
      Out += '_';
  }
  static const char Hex[] = "0123456789ABCDEF";
  Out += '_';
  for (int Shift = 28; Shift >= 0; Shift -= 4)
    Out += Hex[(Hash >> Shift) & 0xF];
  return Out;
}

std::string makeInstantiationBlock(
    const TemplateDef &Template, StringRef TypeSpelling, StringRef GuardMacro,
    ArrayRef<DependencyGuard> ExtraGuards) {
  std::string Out;
  Out.reserve(128 + Template.IncludeFromGenDir.size() + TypeSpelling.size() +
              GuardMacro.size());

  const std::string Marker = instantiationMarker(Template.Name, TypeSpelling);
  Out.append(Marker);
  Out += '\n';

  // The presence macro lets the same nested type be emitted in several headers
  // without a duplicate definition; whichever block is seen first wins.
  const std::string Presence = presenceMacro(Template.Name, TypeSpelling);
  Out += "#ifndef ";
  Out += Presence;
  Out += '\n';

  // A nested generic type is only usable when its generated header has already
  // been included. Wrap the entire block in every dependency guard, plus the
  // type-defining guard when one was found. The presence macro is defined only
  // after those guards pass, so a skipped block does not suppress a fallback
  // copy in another generated header.
  StringSet<> SeenGuards;
  SmallVector<std::string, 4> Guards;
  auto addGuard = [&](StringRef Guard) {
    if (!Guard.empty() && SeenGuards.insert(Guard).second)
      Guards.push_back(Guard.str());
  };
  for (const DependencyGuard &Dep : ExtraGuards)
    addGuard(Dep.Guard);
  addGuard(GuardMacro);

  for (const std::string &Guard : Guards) {
    Out += "#ifdef ";
    Out += Guard;
    Out += '\n';
  }

  Out += "#define ";
  Out += Presence;
  Out += '\n';

  StringRef Param =
      Template.Params.empty() ? StringRef("T") : Template.Params.front();
  Out += "#define ";
  Out += Param;
  Out += ' ';
  Out += TypeSpelling;
  Out += '\n';
  Out += "#include \"";
  Out += Template.IncludeFromGenDir;
  Out += "\"\n";
  Out += "#undef ";
  Out += Param;
  Out += '\n';
  for (unsigned I = Guards.size(); I > 0; --I)
    Out += "#endif\n";
  Out += "#endif\n";
  Out += '\n';
  return Out;
}

void Instantiator::loadState(FileState &State, StringRef Path,
                             uint64_t Stamp) {
  State.Content.clear();
  State.Markers.clear();
  State.Stamp = Stamp;

  auto BufferOrErr = MemoryBuffer::getFile(Path, /*IsText=*/true);
  if (!BufferOrErr)
    return;

  StringRef Buffer = (*BufferOrErr)->getBuffer();
  State.Content = Buffer.str();
  StringRef Rest = Buffer;
  while (!Rest.empty()) {
    size_t Begin = Rest.find(MarkerPrefix);
    if (Begin == StringRef::npos)
      break;
    Rest = Rest.drop_front(Begin);
    size_t End = Rest.find(MarkerSuffix);
    if (End == StringRef::npos)
      break;
    State.Markers.insert(Rest.take_front(End + MarkerSuffix.size()));
    Rest = Rest.drop_front(End + MarkerSuffix.size());
  }
}

Instantiator::FileState &Instantiator::getState(StringRef Path) {
  uint64_t Stamp = stampFor(Path);
  auto It = Files.find(Path);
  if (It != Files.end()) {
    if (It->second->Stamp != Stamp)
      loadState(*It->second, Path, Stamp);
    return *It->second;
  }

  auto State = std::make_unique<FileState>();
  loadState(*State, Path, Stamp);
  FileState *Ptr = State.get();
  Files[Path] = std::move(State);
  return *Ptr;
}

bool Instantiator::has(const TemplateDef &Template, StringRef TypeSpelling,
                       StringRef OutputHeader) {
  std::lock_guard<std::mutex> Lock(Mutex);
  StringRef Path =
      OutputHeader.empty() ? Template.GeneratedHeader : OutputHeader;
  FileState &State = getState(Path);
  return State.Markers.contains(instantiationMarker(Template.Name, TypeSpelling)) ||
         hasManualInstantiation(State.Content, Template, TypeSpelling);
}

bool Instantiator::prune(const TemplateDef &Template, const StringSet<> &Keep,
                         std::vector<std::string> &Removed,
                         std::string &Error) {
  SmallVector<const TemplateDef *, 1> Templates{&Template};
  return prune(Templates, Template.GeneratedHeader, Keep, Removed, Error);
}

bool Instantiator::prune(ArrayRef<const TemplateDef *> Templates,
                         StringRef OutputHeader, const StringSet<> &Keep,
                         std::vector<std::string> &Removed,
                         std::string &Error) {
  std::lock_guard<std::mutex> Lock(Mutex);
  FileState &State = getState(OutputHeader);
  if (State.Markers.empty())
    return false;

  StringSet<> Remove;
  for (const auto &Entry : State.Markers)
    if (!Keep.contains(Entry.getKey()))
      Remove.insert(Entry.getKey());

  auto Lines = splitLines(State.Content);
  std::vector<std::pair<size_t, size_t>> Ranges;
  StringSet<> Logged;
  size_t SkipUntil = 0;
  for (size_t I = 0; I < Lines.size(); ++I) {
    if (Lines[I].Begin < SkipUntil)
      continue;

    for (const TemplateDef *Template : Templates) {
      if (!Template)
        continue;
      StringRef Type;
      size_t Begin, End;
      bool Marked = false;
      if (!findBlockAt(Lines, I, *Template, Type, Begin, End, Marked))
        continue;
      SkipUntil = End;

      std::string Marker = instantiationMarker(Template->Name, Type);
      if (Marked) {
        if (Remove.contains(Marker)) {
          Ranges.push_back({Begin, End});
          if (Logged.insert(Marker).second)
            Removed.push_back(std::move(Marker));
        }
      } else if (State.Markers.contains(Marker)) {
        // An older hand-written block is redundant once clangd owns this
        // instantiation. Remove it rather than leaving a duplicate definition.
        Ranges.push_back({Begin, End});
      }
      break;
    }
  }

  if (Ranges.empty())
    return false;

  std::sort(Ranges.begin(), Ranges.end());
  size_t MergedBegin = Ranges.front().first;
  size_t MergedEnd = Ranges.front().second;
  std::vector<std::pair<size_t, size_t>> Merged;
  for (size_t I = 1; I < Ranges.size(); ++I) {
    if (Ranges[I].first <= MergedEnd) {
      MergedEnd = std::max(MergedEnd, Ranges[I].second);
      continue;
    }
    Merged.push_back({MergedBegin, MergedEnd});
    MergedBegin = Ranges[I].first;
    MergedEnd = Ranges[I].second;
  }
  Merged.push_back({MergedBegin, MergedEnd});

  StringRef Content(State.Content);
  std::string NewContent;
  NewContent.reserve(Content.size());
  size_t Pos = 0;
  for (const auto &Range : Merged) {
    NewContent.append(Content.data() + Pos, Range.first - Pos);
    Pos = Range.second;
  }
  NewContent.append(Content.data() + Pos, Content.size() - Pos);

  std::error_code EC;
  raw_fd_ostream OS(OutputHeader, EC, sys::fs::CD_CreateAlways);
  if (EC) {
    Error = EC.message();
    return false;
  }
  OS << NewContent;
  OS.close();
  if (OS.has_error()) {
    Error = "failed writing " + OutputHeader.str();
    return false;
  }

  State.Content = std::move(NewContent);
  State.Stamp = stampFor(OutputHeader);
  for (const std::string &Marker : Removed)
    State.Markers.erase(Marker);
  return true;
}

InstantiationResult
Instantiator::instantiate(const TemplateDef &Template, StringRef TypeSpelling,
                          StringRef GuardMacro, std::string &GenPath,
                          std::string &Error, bool DryRun,
                          ArrayRef<DependencyGuard> ExtraGuards,
                          StringRef OutputHeader) {
  std::lock_guard<std::mutex> Lock(Mutex);
  GenPath = OutputHeader.empty() ? Template.GeneratedHeader : OutputHeader.str();

  FileState &State = getState(GenPath);
  const std::string Marker = instantiationMarker(Template.Name, TypeSpelling);
  if (State.Markers.contains(Marker) ||
      hasManualInstantiation(State.Content, Template, TypeSpelling))
    return InstantiationResult::Existing;

  std::string NewContent;
  NewContent.reserve(State.Content.size() + 160);
  if (State.Content.empty()) {
    std::string FileGuard = generatedHeaderFileGuard(GenPath);
    NewContent += "#ifndef ";
    NewContent += FileGuard;
    NewContent += "\n#define ";
    NewContent += FileGuard;
    NewContent += "\n\n";
    NewContent +=
        makeInstantiationBlock(Template, TypeSpelling, GuardMacro, ExtraGuards);
    NewContent += "#endif\n";
  } else {
    StringRef Content(State.Content);
    size_t Pos = Content.rfind("#endif");
    size_t GuardLineStart = Pos == StringRef::npos
                                ? StringRef::npos
                                : Content.rfind('\n', Pos);
    if (GuardLineStart == StringRef::npos)
      GuardLineStart = 0;
    else
      ++GuardLineStart;

    StringRef GuardLine = Content.drop_front(GuardLineStart);
    if (Pos != StringRef::npos && GuardLine.starts_with("#endif")) {
      size_t LineEnd = Content.find('\n', Pos);
      if (LineEnd == StringRef::npos)
        LineEnd = Content.size();
      GuardLine = Content.slice(GuardLineStart, LineEnd);
    } else {
      GuardLine = {};
    }

    if (!GuardLine.empty()) {
      NewContent.append(Content.data(), GuardLineStart);
      NewContent += makeInstantiationBlock(Template, TypeSpelling, GuardMacro,
                                           ExtraGuards);
      NewContent.append(Content.data() + GuardLineStart,
                        Content.size() - GuardLineStart);
    } else {
      NewContent = Content.str();
      NewContent += '\n';
      NewContent += makeInstantiationBlock(Template, TypeSpelling, GuardMacro,
                                           ExtraGuards);
    }
  }

  if (DryRun) {
    State.Content = std::move(NewContent);
    State.Markers.insert(Marker);
    return InstantiationResult::Added;
  }

  std::error_code EC;
  SmallString<256> Dir(sys::path::parent_path(GenPath));
  if (!Dir.empty())
    EC = sys::fs::create_directories(Dir);
  if (EC) {
    Error = EC.message();
    return InstantiationResult::Error;
  }

  raw_fd_ostream OS(GenPath, EC, sys::fs::CD_CreateAlways);
  if (EC) {
    Error = EC.message();
    return InstantiationResult::Error;
  }
  OS << NewContent;
  OS.close();
  if (OS.has_error()) {
    Error = "failed writing " + GenPath;
    return InstantiationResult::Error;
  }

  State.Content = std::move(NewContent);
  State.Stamp = stampFor(GenPath);
  State.Markers.insert(Marker);
  return InstantiationResult::Added;
}

} // namespace c_autotmpl
