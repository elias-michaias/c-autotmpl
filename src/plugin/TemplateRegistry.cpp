#include "TemplateRegistry.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <cctype>
#include <tuple>

using namespace llvm;

namespace c_autotmpl {

namespace {

bool isIdentStart(char C) {
  return C == '_' || (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z');
}

bool isIdentChar(char C) {
  return isIdentStart(C) || (C >= '0' && C <= '9');
}

bool isSpace(char C) {
  return C == ' ' || C == '\t' || C == '\r' || C == '\n' || C == '\f' ||
         C == '\v';
}

StringRef stripStcQualifiers(StringRef S) {
  S = S.trim();
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (StringRef Q : {"const", "volatile", "restrict", "struct", "union",
                        "enum"}) {
      if (S.consume_front(Q)) {
        S = S.trim();
        Changed = true;
      }
    }
  }
  return S;
}

bool parseDecorator(StringRef Line, TemplateDef &Out) {
  StringRef S = Line.ltrim();
  if (!S.consume_front("//"))
    return false;
  S = S.ltrim();
  if (!S.consume_front("--"))
    return false;
  S = S.ltrim();
  if (!S.consume_front("template"))
    return false;
  if (!S.empty() && !isSpace(S.front()))
    return false;
  S = S.ltrim();

  TemplateKind Kind = TemplateKind::Type;
  size_t Word = 0;
  while (Word < S.size() && isIdentChar(S[Word]))
    ++Word;
  if (S.starts_with("type") && Word == 4 &&
      (S.size() == 4 || isSpace(S[4]))) {
    S = S.drop_front(4).ltrim();
  } else if (S.starts_with("function") && Word == 8 &&
             (S.size() == 8 || isSpace(S[8]))) {
    Kind = TemplateKind::Function;
    S = S.drop_front(8).ltrim();
  }

  size_t I = 0;
  while (I < S.size() && isIdentChar(S[I]))
    ++I;
  if (I == 0)
    return false;
  Out.Kind = Kind;
  Out.Name = S.take_front(I).str();
  S = S.drop_front(I).ltrim();
  if (!S.consume_front("("))
    return false;

  Out.Params.clear();
  size_t Close = S.find(')');
  if (Close == StringRef::npos)
    return false;
  StringRef Params = S.take_front(Close);
  while (!Params.empty()) {
    StringRef P = Params.split(',').first;
    P = P.trim();
    if (!P.empty())
      Out.Params.push_back(P.str());
    Params = Params.split(',').second;
  }
  return true;
}

bool isHeaderExtension(StringRef Ext) {
  return Ext == ".h" || Ext == ".hh" || Ext == ".hpp" || Ext == ".hxx";
}

bool skipDirectory(StringRef Component) {
  return Component == ".git" || Component == "build" ||
         Component == "node_modules" ||
         (!Component.empty() && Component.front() == '.');
}

std::string lowerCopy(StringRef S) {
  std::string Out(S);
  for (char &C : Out)
    if (C >= 'A' && C <= 'Z')
      C = static_cast<char>(C - 'A' + 'a');
  return Out;
}

std::string genHeaderPath(StringRef TemplateHeader, StringRef Name) {
  SmallString<256> Parent(sys::path::parent_path(TemplateHeader));
  StringRef ParentName = sys::path::filename(Parent);
  SmallString<256> GenDir;
  if (ParentName == "tmpl" || ParentName == "templates") {
    GenDir = sys::path::parent_path(Parent);
  } else {
    GenDir = Parent;
  }
  sys::path::append(GenDir, "gen");
  sys::path::append(GenDir, lowerCopy(Name) + ".h");
  sys::path::remove_dots(GenDir, /*remove_dot_dot=*/true);
  return std::string(GenDir);
}

std::string genImplPath(StringRef TemplateHeader, StringRef Name) {
  SmallString<256> Parent(sys::path::parent_path(TemplateHeader));
  StringRef ParentName = sys::path::filename(Parent);
  SmallString<256> ImplDir;
  if (ParentName == "tmpl" || ParentName == "templates") {
    ImplDir = sys::path::parent_path(Parent);
  } else {
    ImplDir = Parent;
  }
  sys::path::append(ImplDir, "gen");
  sys::path::append(ImplDir, "impl");
  sys::path::append(ImplDir, lowerCopy(Name) + ".c");
  sys::path::remove_dots(ImplDir, /*remove_dot_dot=*/true);
  return std::string(ImplDir);
}

StringRef readMacroName(StringRef S) {
  S = S.ltrim();
  size_t I = 0;
  while (I < S.size() && isIdentChar(S[I]))
    ++I;
  return S.take_front(I);
}

StringRef guardMacroFromLine(StringRef Line) {
  StringRef S = Line.ltrim();

  if (S.consume_front("#ifndef"))
    return readMacroName(S);

  if (!S.consume_front("#if"))
    return {};
  S = S.ltrim();
  if (!S.consume_front("!"))
    return {};
  S = S.ltrim();
  StringRef Probe = S;
  if (!Probe.consume_front("defined"))
    return {};
  if (Probe.empty() || (Probe.front() != '(' && !isSpace(Probe.front())))
    return {};
  S = Probe.ltrim();
  S.consume_front("(");
  return readMacroName(S);
}

bool definesMacroSoon(StringRef Rest, StringRef Macro) {
  for (int I = 0; I < 8 && !Rest.empty(); ++I) {
    StringRef Line;
    std::tie(Line, Rest) = Rest.split('\n');
    StringRef S = Line.ltrim();
    if (S.empty() || S.starts_with("//") || S.starts_with("/*"))
      continue;
    if (!S.consume_front("#define"))
      continue;
    if (readMacroName(S) == Macro)
      return true;
  }
  return false;
}

bool hasTmplHeaderGuard(StringRef Buffer) {
  bool InBlockComment = false;
  while (!Buffer.empty()) {
    StringRef Line;
    std::tie(Line, Buffer) = Buffer.split('\n');
    StringRef S = Line.ltrim();
    if (InBlockComment) {
      if (S.contains("*/"))
        InBlockComment = false;
      continue;
    }
    if (S.starts_with("//"))
      continue;
    if (S.starts_with("/*")) {
      if (!S.contains("*/"))
        InBlockComment = true;
      continue;
    }
    if (guardMacroFromLine(Line) == "TMPL_HEADER")
      return true;
  }
  return false;
}

ArgSpec stcArgSpec(StringRef Param) {
  struct Mapping {
    StringRef Name;
    ArgRole Role;
    const char *Macro;
  };
  static const Mapping Mappings[] = {
      {"Name", ArgRole::Name, "i_type"},
      {"Container", ArgRole::Name, "i_type"},
      {"Ct", ArgRole::Name, "i_type"},
      {"Key", ArgRole::Key, "i_key"},
      {"K", ArgRole::Key, "i_key"},
      {"Elem", ArgRole::Key, "i_key"},
      {"Element", ArgRole::Key, "i_key"},
      {"T", ArgRole::Key, "i_key"},
      {"Val", ArgRole::Value, "i_val"},
      {"Value", ArgRole::Value, "i_val"},
      {"V", ArgRole::Value, "i_val"},
      {"Traits", ArgRole::Flags, "i_traits"},
      {"Flags", ArgRole::Flags, "i_traits"},
      {"Opts", ArgRole::Flags, "i_traits"},
      {"Options", ArgRole::Flags, "i_traits"},
      {"Alloc", ArgRole::Opaque, "i_allocator"},
      {"Allocator", ArgRole::Opaque, "i_allocator"},
      {"Cap", ArgRole::Opaque, "i_capacity"},
      {"Capacity", ArgRole::Opaque, "i_capacity"},
  };
  for (const Mapping &M : Mappings)
    if (Param == M.Name)
      return {M.Role, M.Macro};
  return {ArgRole::Type, Param.str()};
}

} // namespace

bool isTypeArgument(const TemplateDef &Template, size_t Index) {
  if (Index >= Template.ArgSpecs.size())
    return true;
  ArgRole Role = Template.ArgSpecs[Index].Role;
  return Role == ArgRole::Type || Role == ArgRole::Key ||
         Role == ArgRole::Value;
}

StringRef parameterName(const TemplateDef &Template, size_t Index) {
  if (Index < Template.Params.size())
    return Template.Params[Index];
  return StringRef("T");
}

StringRef argumentMacroName(const TemplateDef &Template, size_t Index) {
  if (Index < Template.ArgSpecs.size() &&
      !Template.ArgSpecs[Index].Macro.empty())
    return Template.ArgSpecs[Index].Macro;
  return parameterName(Template, Index);
}

bool isStcPreset(const TemplateDef &Template) {
  return Template.Preset == TemplatePreset::StcStatic ||
         Template.Preset == TemplatePreset::StcShared;
}

bool parsePresetName(StringRef Name, TemplatePreset &Out) {
  if (Name == "none") {
    Out = TemplatePreset::None;
    return true;
  }
  if (Name == "stc" || Name == "stc-static") {
    Out = TemplatePreset::StcStatic;
    return true;
  }
  if (Name == "stc-shared") {
    Out = TemplatePreset::StcShared;
    return true;
  }
  return false;
}

std::string makeAbsolute(StringRef Path) {
  SmallString<256> P(Path);
  std::error_code EC = sys::fs::make_absolute(P);
  if (EC)
    return std::string(Path);
  sys::path::remove_dots(P, true);
  return std::string(P);
}

std::string computeRelativePath(StringRef FromDir, StringRef ToFile) {
  SmallVector<StringRef, 8> From, To;
  for (auto It = sys::path::begin(FromDir), End = sys::path::end(FromDir);
       It != End; ++It)
    if (!It->empty())
      From.push_back(*It);
  for (auto It = sys::path::begin(ToFile), End = sys::path::end(ToFile);
       It != End; ++It)
    if (!It->empty())
      To.push_back(*It);

  size_t Common = 0;
  while (Common < From.size() && Common < To.size() && From[Common] == To[Common])
    ++Common;

  std::string Out;
  for (size_t I = Common; I < From.size(); ++I)
    Out += "../";
  for (size_t I = Common; I < To.size(); ++I) {
    if (!Out.empty() && Out.back() != '/')
      Out += '/';
    Out += To[I].str();
  }
  return Out.empty() ? std::string(".") : Out;
}

void TemplateRegistry::scanTree(StringRef Dir) {
  std::error_code EC;
  for (sys::fs::recursive_directory_iterator It(Dir, EC,
                                                  /*follow_symlinks=*/false),
       End;
       It != End && !EC; It.increment(EC)) {
    if (It->type() == sys::fs::file_type::directory_file) {
      if (skipDirectory(sys::path::filename(It->path())))
        It.no_push();
      continue;
    }
    if (It->type() != sys::fs::file_type::regular_file)
      continue;
    if (!isHeaderExtension(sys::path::extension(It->path())))
      continue;

    auto BufferOrErr = MemoryBuffer::getFile(It->path(), /*IsText=*/true);
    if (!BufferOrErr)
      continue;
    StringRef Buffer = (*BufferOrErr)->getBuffer();
    bool HasGuard = hasTmplHeaderGuard(Buffer);

    auto Def = std::make_unique<TemplateDef>();
    bool Found = false;
    while (!Buffer.empty()) {
      StringRef Line;
      std::tie(Line, Buffer) = Buffer.split('\n');
      if (parseDecorator(Line, *Def)) {
        Found = true;
        break;
      }
    }
    if (!Found)
      continue;

    Def->Header = makeAbsolute(It->path());
    Def->GeneratedHeader = genHeaderPath(Def->Header, Def->Name);
    Def->GeneratedImplSource = genImplPath(Def->Header, Def->Name);
    Def->HasTmplHeaderGuard = HasGuard;
    Def->IncludeFromGenDir =
        computeRelativePath(sys::path::parent_path(Def->GeneratedHeader),
                            Def->Header);
    Def->IncludeFromImplDir =
        computeRelativePath(sys::path::parent_path(Def->GeneratedImplSource),
                            Def->Header);

    const TemplateDef *Ptr = Def.get();
    Owned.push_back(std::move(Def));
    ByName[Ptr->Name] = Ptr;
    ByLength.push_back(Ptr);
  }
}

void TemplateRegistry::finishScan() {
  std::stable_sort(ByLength.begin(), ByLength.end(),
                   [](const TemplateDef *A, const TemplateDef *B) {
                     return A->Name.size() > B->Name.size();
                   });
}

void TemplateRegistry::scan(StringRef Root) {
  Owned.clear();
  ByName.clear();
  ByLength.clear();
  this->Root = Root.str();
  if (sys::fs::is_directory(Root))
    scanTree(Root);
  finishScan();
}

void TemplateRegistry::scanPreferTemplateDirs(StringRef Root) {
  Owned.clear();
  ByName.clear();
  ByLength.clear();
  this->Root = Root.str();
  if (!sys::fs::is_directory(Root)) {
    finishScan();
    return;
  }

  for (StringRef Name : {"tmpl", "templates"}) {
    SmallString<256> Dir(Root);
    sys::path::append(Dir, Name);
    if (!sys::fs::is_directory(Dir))
      continue;
    scanTree(Dir);
    if (!Owned.empty()) {
      finishScan();
      return;
    }
  }

  // Unconventional layouts still work; pay for the full walk only when the
  // conventional directories contain no decorators.
  scanTree(Root);
  finishScan();
}

bool looksLikeStcHeader(const TemplateDef &Template) {
  auto Buffer = MemoryBuffer::getFile(Template.Header, /*IsText=*/true);
  if (!Buffer)
    return false;
  StringRef Content = (*Buffer)->getBuffer();
  static const char *Markers[] = {"i_key",       "i_val",       "i_type",
                                  "i_traits",    "i_implement", "i_header",
                                  "i_capacity",  "i_allocator"};
  int Score = Content.contains("stc/") ? 2 : 0;
  for (const char *Marker : Markers)
    if (Content.contains(Marker))
      ++Score;
  return Score >= 2;
}

void TemplateRegistry::applyPreset(TemplatePreset Preset,
                                   ArrayRef<StringRef> Names, bool Auto) {
  auto Selected = [&](const TemplateDef &Def) {
    if (Auto && looksLikeStcHeader(Def))
      return true;
    if (Names.empty())
      return StringRef(Def.Name).starts_with("stc_");
    for (StringRef Name : Names)
      if (Name == Def.Name)
        return true;
    return false;
  };
  for (auto &OwnedDef : Owned) {
    OwnedDef->ArgSpecs.clear();
    if (Preset == TemplatePreset::None || !Selected(*OwnedDef)) {
      OwnedDef->Preset = TemplatePreset::None;
      continue;
    }
    OwnedDef->Preset = Preset;
    size_t Count = OwnedDef->Params.empty() ? 1 : OwnedDef->Params.size();
    for (size_t I = 0; I < Count; ++I)
      OwnedDef->ArgSpecs.push_back(
          stcArgSpec(parameterName(*OwnedDef, I)));
  }
}

bool usesDerivedStcNames(const TemplateDef &Template) {
  if (!isStcPreset(Template))
    return false;
  for (const ArgSpec &Spec : Template.ArgSpecs)
    if (Spec.Role == ArgRole::Name || Spec.Role == ArgRole::Flags ||
        Spec.Role == ArgRole::Opaque)
      return false;
  return true;
}

bool isStcBuiltinType(StringRef Spelling) {
  StringRef Name = stripStcQualifiers(Spelling);
  static const char *Names[] = {"cstr",    "csview",     "csviewz",
                                "zsview",  "cstr_buf",   "cstr_raw",
                                "cstr_value", "csview_value", "csviewz_value",
                                "cbits",   "coption",    "cbox",
                                "carc",    "crc"};
  for (const char *Builtin : Names)
    if (Name == Builtin)
      return true;
  return false;
}

static bool isStcStringType(StringRef Spelling) {
  StringRef Name = stripStcQualifiers(Spelling);
  return Name == "cstr" || Name == "csview" || Name == "csviewz" ||
         Name == "zsview";
}

std::string stcContainerName(const TemplateDef &Template,
                             ArrayRef<std::string> Args) {
  static const char Hex[] = "0123456789ABCDEF";
  std::string Out = Template.Name;
  for (const std::string &Raw : Args) {
    StringRef Arg = StringRef(Raw).trim();
    Out += '_';
    for (char C : Arg) {
      unsigned char Byte = static_cast<unsigned char>(C);
      if (isIdentChar(C)) {
        Out += C;
      } else {
        Out += '_';
        Out += Hex[Byte >> 4];
        Out += Hex[Byte & 0xF];
      }
    }
  }
  return Out;
}

std::string stcTraits(const TemplateDef &Template,
                      ArrayRef<std::string> Args) {
  if (!usesDerivedStcNames(Template))
    return {};
  bool Key = false, Value = false;
  for (size_t I = 0; I < Args.size(); ++I) {
    if (!isStcStringType(Args[I]))
      continue;
    ArgRole Role = I < Template.ArgSpecs.size() ? Template.ArgSpecs[I].Role
                                                : ArgRole::Type;
    if (Role == ArgRole::Value)
      Value = true;
    else
      Key = true;
  }
  if (!Key && !Value)
    return {};
  std::string Out = "(";
  if (Key)
    Out += "c_pro_key";
  if (Value) {
    if (Key)
      Out += " | ";
    Out += "c_pro_val";
  }
  if (Key)
    Out += " | c_use_eq";
  Out += ')';
  return Out;
}

std::string findIncludeGuard(StringRef Path) {
  auto BufferOrErr = MemoryBuffer::getFile(Path, /*IsText=*/true);
  if (!BufferOrErr)
    return {};
  return findIncludeGuardInBuffer((*BufferOrErr)->getBuffer());
}

std::string findIncludeGuardInBuffer(StringRef Buffer) {
  // Require a following `#define MACRO`, so unrelated conditionals near the
  // top of a header (e.g. `#ifndef __cplusplus`) are not mistaken for guards.
  for (int LineNo = 0; LineNo < 80 && !Buffer.empty(); ++LineNo) {
    StringRef Line;
    std::tie(Line, Buffer) = Buffer.split('\n');
    StringRef Macro = guardMacroFromLine(Line);
    if (!Macro.empty() && definesMacroSoon(Buffer, Macro))
      return Macro.str();
  }
  return {};
}

std::string generatedHeaderFileGuard(StringRef GenPath) {
  std::string Guard = findIncludeGuard(GenPath);
  if (!Guard.empty())
    return Guard;

  SmallString<128> Out;
  StringRef Parent = sys::path::filename(sys::path::parent_path(GenPath));
  StringRef Stem = sys::path::stem(GenPath);
  Out += Parent;
  Out += '_';
  Out += Stem;
  for (char &C : Out) {
    if (C >= 'a' && C <= 'z')
      C = static_cast<char>(C - 'a' + 'A');
    else if (!((C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9')))
      C = '_';
  }
  Out += "_H";
  return std::string(Out);
}

} // namespace c_autotmpl
