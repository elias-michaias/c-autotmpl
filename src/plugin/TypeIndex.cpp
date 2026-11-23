#include "TypeIndex.h"

#include "SourceScan.h"
#include "TemplateRegistry.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

using namespace llvm;

namespace c_autotmpl {
namespace {

bool isIdentStart(char C) {
  return C == '_' || (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z');
}

bool isHeader(StringRef Ext) {
  return Ext == ".h" || Ext == ".hh" || Ext == ".hpp" || Ext == ".hxx";
}

bool skipDirectory(StringRef Name) {
  return Name == "build" || Name == "node_modules" || Name == "gen" ||
         (!Name.empty() && Name.front() == '.');
}

bool isKeyword(StringRef S) {
  static const StringRef Keywords[] = {
      "struct", "union",  "enum",     "class",    "const",
      "volatile", "restrict", "static", "inline", "extern",
      "register", "auto",  "signed",   "unsigned", "long",
      "short",    "int",   "char",     "float",    "double",
      "void",     "_Bool", "typedef"};
  for (StringRef K : Keywords)
    if (S == K)
      return true;
  return false;
}

void addDeclaredName(StringMap<std::string> &Guards,
                     StringMap<std::string> &Headers, StringRef Name,
                     StringRef Guard, StringRef Header) {
  if (Name.empty() || !isIdentStart(Name.front()) || Guard.empty())
    return;
  if (isKeyword(Name) || isBuiltinTypeName(Name))
    return;
  if (!Guards.count(Name)) {
    Guards[Name] = Guard.str();
    Headers[Name] = Header.str();
  }
}

void indexHeader(StringRef Buffer, StringRef Guard, StringRef Header,
                 StringMap<std::string> &Guards,
                 StringMap<std::string> &Headers) {
  std::string Clean = stripCommentsAndLiterals(Buffer);
  SmallVector<Token, 128> Tokens;
  tokenize(Clean, Tokens);

  for (size_t I = 0; I < Tokens.size(); ++I) {
    StringRef T = Tokens[I].Text;

    if ((T == "struct" || T == "union" || T == "enum" || T == "class") &&
        I + 1 < Tokens.size()) {
      addDeclaredName(Guards, Headers, Tokens[I + 1].Text, Guard, Header);
      continue;
    }

    if (T != "typedef")
      continue;

    int BraceDepth = 0;
    StringRef LastName;
    auto flush = [&]() {
      addDeclaredName(Guards, Headers, LastName, Guard, Header);
      LastName = {};
    };

    for (size_t J = I + 1; J < Tokens.size(); ++J) {
      StringRef U = Tokens[J].Text;
      if (U == "{") {
        ++BraceDepth;
        continue;
      }
      if (U == "}") {
        if (BraceDepth > 0)
          --BraceDepth;
        continue;
      }
      if (U == ";" && BraceDepth == 0) {
        flush();
        break;
      }
      if (U == "," && BraceDepth == 0) {
        flush();
        continue;
      }
      if (isIdentStart(U.front()) && !isKeyword(U))
        LastName = U;
    }
  }
}

} // namespace

void TypeIndex::scan(StringRef Root, ArrayRef<std::string> TypeNames) {
  Guards.clear();
  Headers.clear();
  std::error_code EC;
  if (!sys::fs::is_directory(Root))
    return;

  StringSet<> Remaining;
  for (const std::string &Name : TypeNames) {
    StringRef Ident = lastIdentifier(Name);
    if (!Ident.empty())
      Remaining.insert(Ident.str());
  }
  bool Filtered = !TypeNames.empty();

  for (sys::fs::recursive_directory_iterator It(Root, EC,
                                                  /*follow_symlinks=*/false),
       End;
       It != End && !EC; It.increment(EC)) {
    if (Filtered && Remaining.empty())
      break;
    if (It->type() == sys::fs::file_type::directory_file) {
      if (skipDirectory(sys::path::filename(It->path())))
        It.no_push();
      continue;
    }
    if (It->type() != sys::fs::file_type::regular_file)
      continue;
    if (!isHeader(sys::path::extension(It->path())))
      continue;

    auto BufferOrErr = MemoryBuffer::getFile(It->path(), /*IsText=*/true);
    if (!BufferOrErr)
      continue;
    StringRef Buffer = (*BufferOrErr)->getBuffer();
    std::string Guard = findIncludeGuardInBuffer(Buffer);
    if (Guard.empty())
      continue; // #pragma once or no recognisable guard.

    if (Filtered) {
      bool Relevant = false;
      for (const auto &Entry : Remaining)
        if (Buffer.find(Entry.getKey()) != StringRef::npos) {
          Relevant = true;
          break;
        }
      if (!Relevant)
        continue;
    }

    indexHeader(Buffer, Guard, makeAbsolute(It->path()), Guards, Headers);

    if (Filtered) {
      SmallVector<std::string, 8> Found;
      for (const auto &Entry : Remaining)
        if (Guards.count(Entry.getKey()))
          Found.push_back(Entry.getKey().str());
      for (const std::string &Name : Found)
        Remaining.erase(Name);
    }
  }
}

std::string TypeIndex::guardForType(StringRef TypeSpelling) const {
  StringRef Name = lastIdentifier(TypeSpelling);
  if (Name.empty() || isBuiltinTypeName(Name))
    return {};
  auto It = Guards.find(Name);
  return It == Guards.end() ? std::string() : It->second;
}

std::string TypeIndex::headerForType(StringRef TypeSpelling) const {
  StringRef Name = lastIdentifier(TypeSpelling);
  if (Name.empty() || isBuiltinTypeName(Name))
    return {};
  auto It = Headers.find(Name);
  return It == Headers.end() ? std::string() : It->second;
}

} // namespace c_autotmpl
