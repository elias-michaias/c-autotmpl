#include "SourceScan.h"

#include <cctype>

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

} // namespace

StringRef lastIdentifier(StringRef Spelling) {
  size_t End = Spelling.size();
  while (End > 0 && !isIdentChar(Spelling[End - 1]))
    --End;
  size_t Begin = End;
  while (Begin > 0 && isIdentChar(Spelling[Begin - 1]))
    --Begin;
  return Spelling.slice(Begin, End);
}

bool isBuiltinTypeName(StringRef Name) {
  static const StringRef Builtins[] = {
      "void",    "bool",     "char",     "short",    "int",
      "long",    "float",    "double",   "signed",   "unsigned",
      "_Bool",   "size_t",   "ssize_t",  "int8_t",   "uint8_t",
      "int16_t", "uint16_t", "int32_t",  "uint32_t", "int64_t",
      "uint64_t", "intptr_t", "uintptr_t"};
  for (StringRef B : Builtins)
    if (Name == B)
      return true;
  return false;
}

bool needsIncludeGuard(StringRef TypeSpelling) {
  StringRef Name = lastIdentifier(TypeSpelling);
  return !Name.empty() && !isBuiltinTypeName(Name);
}

std::string stripCommentsAndLiterals(StringRef Source) {
  std::string Out(Source);
  auto blank = [&](unsigned &I) {
    if (Out[I] != '\n')
      Out[I] = ' ';
    ++I;
  };

  for (unsigned I = 0; I < Out.size();) {
    if (Out[I] == '/' && I + 1 < Out.size()) {
      if (Out[I + 1] == '/') {
        blank(I);
        blank(I);
        while (I < Out.size() && Out[I] != '\n')
          blank(I);
        continue;
      }
      if (Out[I + 1] == '*') {
        blank(I);
        blank(I);
        while (I < Out.size()) {
          if (Out[I] == '*' && I + 1 < Out.size() && Out[I + 1] == '/') {
            blank(I);
            blank(I);
            break;
          }
          blank(I);
        }
        continue;
      }
    }

    if (Out[I] == '"' || Out[I] == '\'') {
      char Quote = Out[I];
      blank(I);
      while (I < Out.size()) {
        if (Out[I] == '\\') {
          blank(I);
          if (I < Out.size())
            blank(I);
          continue;
        }
        if (Out[I] == Quote) {
          blank(I);
          break;
        }
        blank(I);
      }
      continue;
    }

    ++I;
  }
  return Out;
}

void tokenize(StringRef Clean, SmallVectorImpl<Token> &Out) {
  Out.clear();
  for (unsigned I = 0; I < Clean.size();) {
    if (isSpace(Clean[I])) {
      ++I;
      continue;
    }
    if (isIdentStart(Clean[I])) {
      unsigned Start = I++;
      while (I < Clean.size() && isIdentChar(Clean[I]))
        ++I;
      Out.push_back({Clean.slice(Start, I), Start});
      continue;
    }
    Out.push_back({Clean.slice(I, I + 1), I});
    ++I;
  }
}

namespace {

/// Parse one type argument from S, handling commas inside nested template or
/// pointer/array syntax. Returns the consumed prefix and sets Rest to the text
/// after the separating comma (or empty when at end).
StringRef splitTypeArg(StringRef S, StringRef &Rest) {
  int Depth = 0;
  for (unsigned I = 0; I < S.size(); ++I) {
    char C = S[I];
    if (C == '(' || C == '<' || C == '[')
      ++Depth;
    else if (C == ')' || C == '>' || C == ']') {
      if (Depth > 0)
        --Depth;
    } else if (C == ',' && Depth == 0) {
      Rest = S.drop_front(I + 1);
      return S.take_front(I);
    }
  }
  Rest = {};
  return S;
}

} // namespace

std::string canonicalizeType(StringRef Type, const TemplateRegistry &Registry,
                             SmallVectorImpl<FoundUsage> &Deps) {
  std::string Clean = stripCommentsAndLiterals(Type);
  StringRef S(Clean);

  unsigned I = 0;
  while (I < S.size() && isSpace(S[I]))
    ++I;

  // Mangle only direct template applications, e.g. `Box(int)` or
  // `struct List(int)`. Arbitrary declarators like `int (*)(Box(int))` are
  // left as-is and their inner applications are discovered as ordinary usages.
  while (I < S.size()) {
    while (I < S.size() && isSpace(S[I]))
      ++I;
    unsigned Start = I;
    if (I >= S.size() || !isIdentStart(S[I]))
      break;
    ++I;
    while (I < S.size() && isIdentChar(S[I]))
      ++I;
    StringRef Word = S.slice(Start, I);
    if (Word == "struct" || Word == "union" || Word == "enum" ||
        Word == "class" || Word == "const" || Word == "volatile" ||
        Word == "restrict") {
      continue;
    }

    unsigned K = I;
    while (K < S.size() && isSpace(S[K]))
      ++K;
    if (K >= S.size() || S[K] != '(')
      break;

    unsigned ArgStart = K + 1;
    int Depth = 1;
    unsigned Close = ArgStart;
    for (; Close < S.size() && Depth > 0; ++Close) {
      if (S[Close] == '(')
        ++Depth;
      else if (S[Close] == ')')
        --Depth;
    }
    if (Depth != 0)
      break;

    const TemplateDef *Def = Registry.findByName(Word);
    unsigned Rest = Close;
    while (Rest < S.size() && isSpace(S[Rest]))
      ++Rest;
    if (!Def || Rest != S.size())
      break;

    StringRef Args = S.slice(ArgStart, Close - 1);
    StringRef RestArgs = Args;
    unsigned DepStart = Deps.size();
    SmallVector<std::string, 4> CanonArgs;
    while (!RestArgs.empty()) {
      StringRef Next;
      StringRef Arg = splitTypeArg(RestArgs, Next).trim();
      RestArgs = Next.ltrim();
      if (Arg.empty())
        return Type.trim().str();
      CanonArgs.push_back(canonicalizeType(Arg, Registry, Deps));
    }
    if (CanonArgs.empty())
      return Type.trim().str();

    std::string Canon = CanonArgs.front();
    for (unsigned J = 1; J < CanonArgs.size(); ++J) {
      Canon += ", ";
      Canon += CanonArgs[J];
    }

    FoundUsage Dep;
    Dep.Template = Def;
    Dep.TypeSpelling = Canon;
    for (unsigned J = DepStart; J < Deps.size(); ++J)
      Dep.DependsOn.push_back({Deps[J].Template, Deps[J].TypeSpelling});
    Deps.push_back(std::move(Dep));

    std::string Mangled = Def->Name;
    Mangled += '_';
    Mangled += CanonArgs.front();
    for (unsigned J = 1; J < CanonArgs.size(); ++J) {
      Mangled += '_';
      Mangled += CanonArgs[J];
    }

    StringRef Prefix = S.take_front(Start).trim();
    if (Prefix.empty())
      return Mangled;
    return Prefix.str() + " " + Mangled;
  }

  // Fall back to the normalized trimmed spelling for non-template types.
  std::string Out;
  Out.reserve(S.size());
  bool InSpace = false;
  bool Started = false;
  for (char C : S) {
    if (isSpace(C)) {
      if (Started)
        InSpace = true;
      continue;
    }
    if (InSpace) {
      Out += ' ';
      InSpace = false;
    }
    Out += C;
    Started = true;
  }

  // Recognize an already-mangled generic argument such as Box_int. This lets
  // callers write List(Box_int) without needing call-site alias macros; the
  // inner Box(int) instantiation is scheduled before the outer block.
  StringRef MangledArg;
  if (const TemplateDef *Def = Registry.matchMangled(Out, MangledArg)) {
    unsigned DepStart = Deps.size();
    std::string CanonArg = canonicalizeType(MangledArg, Registry, Deps);
    FoundUsage Dep;
    Dep.Template = Def;
    Dep.TypeSpelling = std::move(CanonArg);
    for (unsigned J = DepStart; J < Deps.size(); ++J)
      Dep.DependsOn.push_back({Deps[J].Template, Deps[J].TypeSpelling});
    Deps.push_back(std::move(Dep));
  }
  return Out;
}

void scanUsages(StringRef Code, const TemplateRegistry &Registry,
                SmallVectorImpl<FoundUsage> &Out) {
  if (!Registry.mayContainUsage(Code))
    return;

  std::string Clean = stripCommentsAndLiterals(Code);
  StringRef S(Clean);

  for (unsigned I = 0; I < S.size();) {
    // Ignore preprocessor lines: they contain macro definitions such as
    // `#define List(T) List_##T`, not template usages.
    if (I == 0 || S[I - 1] == '\n') {
      unsigned K = I;
      while (K < S.size() && (S[K] == ' ' || S[K] == '\t'))
        ++K;
      if (K < S.size() && S[K] == '#') {
        while (I < S.size() && S[I] != '\n')
          ++I;
        continue;
      }
    }

    if (!isIdentStart(S[I])) {
      ++I;
      continue;
    }

    unsigned End = I + 1;
    while (End < S.size() && isIdentChar(S[End]))
      ++End;
    StringRef Ident = S.slice(I, End);

    if (const TemplateDef *Def = Registry.findByName(Ident)) {
      unsigned K = End;
      while (K < S.size() && isSpace(S[K]))
        ++K;
      if (K < S.size() && S[K] == '(') {
        unsigned Start = K + 1;
        int Depth = 1;
        unsigned Close = Start;
        for (; Close < S.size() && Depth > 0; ++Close) {
          if (S[Close] == '(')
            ++Depth;
          else if (S[Close] == ')')
            --Depth;
        }
        if (Depth == 0) {
          StringRef Type = S.slice(Start, Close - 1).trim();
          if (!Type.empty()) {
            scanTemplateUsage(*Def, Type, Registry, Out);
            I = Close;
            continue;
          }
        }
      }
    }

    StringRef MangledType;
    if (const TemplateDef *Def = Registry.matchMangled(Ident, MangledType))
      scanTemplateUsage(*Def, MangledType, Registry, Out);

    I = End;
  }
}

void scanTemplateUsage(const TemplateDef &Template, StringRef RawType,
                       const TemplateRegistry &Registry,
                       SmallVectorImpl<FoundUsage> &Out) {
  SmallVector<FoundUsage, 4> Expanded;
  std::string Canon = canonicalizeType(RawType, Registry, Expanded);
  for (FoundUsage &Dep : Expanded)
    Dep.IsNested = true;
  Out.append(Expanded.begin(), Expanded.end());

  FoundUsage Usage;
  Usage.Template = &Template;
  Usage.TypeSpelling = std::move(Canon);
  for (const FoundUsage &Dep : Expanded)
    Usage.DependsOn.push_back({Dep.Template, Dep.TypeSpelling});
  Out.push_back(std::move(Usage));
}

} // namespace c_autotmpl
