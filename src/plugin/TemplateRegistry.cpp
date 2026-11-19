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

  if (S.consume_front("type")) {
    if (!S.empty() && !isSpace(S.front()))
      return false;
    S = S.ltrim();
  }

  size_t I = 0;
  while (I < S.size() && isIdentChar(S[I]))
    ++I;
  if (I == 0)
    return false;
  Out.Name = S.take_front(I).str();
  S = S.drop_front(I).ltrim();
  if (!S.consume_front("("))
    return false;

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

} // namespace

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
    Def->IncludeFromGenDir =
        computeRelativePath(sys::path::parent_path(Def->GeneratedHeader),
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
