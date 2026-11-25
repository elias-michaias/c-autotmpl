#include "SourceScan.h"

#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <cctype>

using namespace llvm;

namespace c_autotmpl {

static void scanUsagesImpl(llvm::StringRef Code,
                           const TemplateRegistry &Registry,
                           llvm::SmallVectorImpl<FoundUsage> &Out,
                           bool ScanHeaders);

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

bool isTemplateConstructorArgument(StringRef Spelling,
                                   const TemplateRegistry &Registry) {
  StringRef Name = Spelling.trim();
  if (Name.empty())
    return false;
  for (char C : Name)
    if (!isIdentChar(C))
      return false;
  const TemplateDef *Def = Registry.findByName(Name);
  return Def && Def->Kind == TemplateKind::Type;
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

bool splitExplicitArgs(StringRef Raw, const TemplateDef &Template,
                       SmallVectorImpl<std::string> &Out) {
  Out.clear();
  if (Template.Kind == TemplateKind::Type) {
    if (Template.Params.size() <= 1) {
      StringRef Arg = Raw.trim();
      if (Arg.empty())
        return false;
      Out.push_back(Arg.str());
      return true;
    }

    StringRef Rest = Raw;
    while (!Rest.empty()) {
      StringRef Next;
      StringRef Arg = splitTypeArg(Rest, Next).trim();
      if (Arg.empty())
        return false;
      Out.push_back(Arg.str());
      Rest = Next.ltrim();
    }
    return Out.size() == Template.Params.size();
  }

  StringRef Rest = Raw;
  while (!Rest.empty()) {
    StringRef Next;
    StringRef Arg = splitTypeArg(Rest, Next).trim();
    if (Arg.empty())
      return false;
    Out.push_back(Arg.str());
    Rest = Next.ltrim();
  }
  size_t Expected = Template.Params.empty() ? 1 : Template.Params.size();
  return Out.size() == Expected;
}

std::string joinExplicitArgs(ArrayRef<std::string> Args) {
  std::string Out;
  for (size_t I = 0; I < Args.size(); ++I) {
    if (I)
      Out += ", ";
    Out += Args[I];
  }
  return Out;
}

static std::string substituteParams(StringRef Source,
                                    const TemplateDef &Template,
                                    ArrayRef<std::string> Args) {
  std::string Out;
  Out.reserve(Source.size());
  for (unsigned I = 0; I < Source.size();) {
    if (isIdentStart(Source[I])) {
      unsigned Start = I++;
      while (I < Source.size() && isIdentChar(Source[I]))
        ++I;
      StringRef Ident = Source.slice(Start, I);
      bool Replaced = false;
      for (size_t P = 0;
           P < Template.Params.size() && P < Args.size(); ++P) {
        if (Ident == StringRef(Template.Params[P])) {
          Out += Args[P];
          Replaced = true;
          break;
        }
      }
      if (!Replaced)
        Out.append(Ident.data(), Ident.size());
      continue;
    }
    Out += Source[I++];
  }
  return Out;
}

static void scanHeaderDeps(const TemplateDef &Template,
                           ArrayRef<std::string> Args,
                           const TemplateRegistry &Registry,
                           SmallVectorImpl<FoundUsage> &Out) {
  if (Template.Header.empty() || Args.empty())
    return;
  auto Buf = MemoryBuffer::getFile(Template.Header, /*IsText=*/true);
  if (!Buf)
    return;

  std::string Source = substituteParams((*Buf)->getBuffer(), Template, Args);
  SmallVector<FoundUsage, 4> Header;
  scanUsagesImpl(Source, Registry, Header, /*ScanHeaders=*/false);

  // The substituted header spells the instantiation itself, e.g.
  // `list_sum(int)(...)` or `User(Option, int)`. That is the usage we are
  // already emitting, not a dependency.
  std::string Self = joinExplicitArgs(Args);
  Header.erase(std::remove_if(Header.begin(), Header.end(),
                              [&](const FoundUsage &U) {
                                return U.Template == &Template &&
                                       U.TypeSpelling == Self;
                              }),
               Header.end());
  Out.append(Header.begin(), Header.end());
}

std::string canonicalizeType(StringRef Type, const TemplateRegistry &Registry,
                             SmallVectorImpl<FoundUsage> &Deps);

static std::string canonicalizeArgument(StringRef Arg,
                                        const TemplateDef &Template,
                                        size_t Index,
                                        const TemplateRegistry &Registry,
                                        SmallVectorImpl<FoundUsage> &Deps) {
  if (isTypeArgument(Template, Index))
    return canonicalizeType(Arg, Registry, Deps);
  return Arg.trim().str();
}

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
    if (!Def || Def->Kind != TemplateKind::Type || Rest != S.size())
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

namespace {

bool isDeclaredParam(const TemplateDef &Template, StringRef Arg) {
  for (const std::string &Param : Template.Params)
    if (StringRef(Param) == Arg)
      return true;
  return false;
}

void appendUsageChain(const TemplateDef &Template,
                      ArrayRef<std::string> CanonArgs,
                      SmallVectorImpl<FoundUsage> &Expanded,
                      SmallVectorImpl<FoundUsage> &Out) {
  for (FoundUsage &Dep : Expanded)
    Dep.IsNested = true;
  Out.append(Expanded.begin(), Expanded.end());

  FoundUsage Usage;
  Usage.Template = &Template;
  Usage.TypeSpelling = joinExplicitArgs(CanonArgs);
  for (const std::string &Arg : CanonArgs)
    Usage.Args.push_back(Arg);
  for (const FoundUsage &Dep : Expanded)
    Usage.DependsOn.push_back({Dep.Template, Dep.TypeSpelling});
  Out.push_back(std::move(Usage));
}

} // namespace

void scanTemplateUsage(const TemplateDef &Template, StringRef RawType,
                       const TemplateRegistry &Registry,
                       SmallVectorImpl<FoundUsage> &Out, bool ScanHeaders) {
  SmallVector<FoundUsage, 4> Expanded;
  SmallVector<std::string, 4> RawArgs;
  if (!splitExplicitArgs(RawType, Template, RawArgs))
    return;

  // The template header spells its own parameters, e.g. List(T) or
  // User(F, Payload). Those are not instantiations.
  bool Placeholders = !Template.Params.empty();
  for (const std::string &Arg : RawArgs)
    if (!isDeclaredParam(Template, Arg))
      Placeholders = false;
  if (Placeholders)
    return;

  SmallVector<std::string, 4> CanonArgs;
  for (size_t I = 0; I < RawArgs.size(); ++I)
    CanonArgs.push_back(
        canonicalizeArgument(RawArgs[I], Template, I, Registry, Expanded));
  if (ScanHeaders)
    scanHeaderDeps(Template, CanonArgs, Registry, Expanded);
  appendUsageChain(Template, CanonArgs, Expanded, Out);
}

static void scanFunctionUsage(const TemplateDef &Template, StringRef RawType,
                              const TemplateRegistry &Registry,
                              SmallVectorImpl<FoundUsage> &Out,
                              bool ScanHeaders) {
  SmallVector<std::string, 4> RawArgs;
  if (!splitExplicitArgs(RawType, Template, RawArgs))
    return;

  // The definition inside a template header spells its own macro parameters,
  // e.g. `select(T, U)(T a, U b)`. Those are not instantiations.
  bool Placeholders = !Template.Params.empty();
  for (const std::string &Arg : RawArgs)
    if (!isDeclaredParam(Template, Arg))
      Placeholders = false;
  if (Placeholders)
    return;

  SmallVector<FoundUsage, 4> Expanded;
  SmallVector<std::string, 4> CanonArgs;
  for (size_t I = 0; I < RawArgs.size(); ++I)
    CanonArgs.push_back(
        canonicalizeArgument(RawArgs[I], Template, I, Registry, Expanded));
  if (ScanHeaders)
    scanHeaderDeps(Template, CanonArgs, Registry, Expanded);
  appendUsageChain(Template, CanonArgs, Expanded, Out);
}

static std::string DispatchAffix = "$";
static bool DispatchPrefix = true;

void setDispatchAffix(StringRef Affix, bool Prefix) {
  DispatchAffix = Affix.str();
  DispatchPrefix = Prefix;
}

static bool dispatchAffixIsIdent() {
  return !DispatchAffix.empty() && isIdentStart(DispatchAffix.front());
}

static size_t dispatchNonidentPrefixAt(StringRef Source, unsigned Pos) {
  if (!DispatchPrefix || DispatchAffix.empty() || dispatchAffixIsIdent())
    return 0;
  return Source.substr(Pos).starts_with(DispatchAffix) ? DispatchAffix.size()
                                                       : 0;
}

static size_t dispatchNonidentPostfixAt(StringRef Source, unsigned Pos) {
  if (DispatchPrefix || DispatchAffix.empty() || dispatchAffixIsIdent())
    return 0;
  return Source.substr(Pos).starts_with(DispatchAffix) ? DispatchAffix.size()
                                                       : 0;
}

static bool identHasDispatchAffix(StringRef Ident) {
  if (DispatchAffix.empty() || Ident.size() <= DispatchAffix.size() ||
      !dispatchAffixIsIdent())
    return false;
  if (DispatchPrefix)
    return Ident.starts_with(DispatchAffix);
  return Ident.ends_with(DispatchAffix);
}

static StringRef stripDispatchAffix(StringRef Ident) {
  if (!identHasDispatchAffix(Ident))
    return Ident;
  return DispatchPrefix ? Ident.drop_front(DispatchAffix.size())
                        : Ident.drop_back(DispatchAffix.size());
}

static void scanUsagesImpl(StringRef Code, const TemplateRegistry &Registry,
                           SmallVectorImpl<FoundUsage> &Out,
                           bool ScanHeaders) {
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

    size_t PrefixLen = dispatchNonidentPrefixAt(S, I);
    I += static_cast<unsigned>(PrefixLen);
    if (I >= S.size() || !isIdentStart(S[I])) {
      ++I;
      continue;
    }

    unsigned End = I + 1;
    while (End < S.size() && isIdentChar(S[End]))
      ++End;
    StringRef RawIdent = S.slice(I, End);
    if (PrefixLen || dispatchNonidentPostfixAt(S, End) ||
        identHasDispatchAffix(RawIdent)) {
      I = End;
      continue;
    }
    StringRef Ident = stripDispatchAffix(RawIdent);

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
            if (Def->Kind == TemplateKind::Function)
              scanFunctionUsage(*Def, Type, Registry, Out, ScanHeaders);
            else
              scanTemplateUsage(*Def, Type, Registry, Out, ScanHeaders);
            I = Close;
            continue;
          }
        }
      }
    }

    // A generated STC adapter reads as vec_push(int)(...). The leading
    // identifier is a call, not the mangled type vec_push. For STC-derived
    // templates, remember the operation and instantiate the underlying type.
    // Other call-like identifiers keep the old skip behavior.
    unsigned After = End;
    while (After < S.size() && isSpace(S[After]))
      ++After;
    if (After < S.size() && S[After] == '(') {
      unsigned Start = After + 1;
      int Depth = 1;
      unsigned Close = Start;
      for (; Close < S.size() && Depth > 0; ++Close) {
        if (S[Close] == '(')
          ++Depth;
        else if (S[Close] == ')')
          --Depth;
      }
      unsigned Call = Close;
      while (Call < S.size() && isSpace(S[Call]))
        ++Call;
      if (Depth == 0 && Close > Start && Call < S.size() && S[Call] == '(') {
        StringRef MangledType;
        if (const TemplateDef *Def =
                Registry.matchMangled(Ident, MangledType)) {
          if (usesDerivedStcNames(*Def) && !MangledType.empty() &&
              isIdentStart(MangledType.front())) {
            bool ValidOp = true;
            for (char C : MangledType)
              if (!isIdentChar(C))
                ValidOp = false;
            if (ValidOp) {
              StringRef Type = S.slice(Start, Close - 1).trim();
              if (!Type.empty()) {
                size_t Before = Out.size();
                scanTemplateUsage(*Def, Type, Registry, Out, ScanHeaders);
                for (size_t K = Before; K < Out.size(); ++K)
                  if (Out[K].Template == Def) {
                    Out[K].AdapterOp = MangledType.str();
                    break;
                  }
              }
              I = Close;
              continue;
            }
          }
        }
      }
    } else {
      StringRef MangledType;
      if (const TemplateDef *Def = Registry.matchMangled(Ident, MangledType))
        scanTemplateUsage(*Def, MangledType, Registry, Out, ScanHeaders);
    }

    I = End;
  }
}

void scanUsages(StringRef Code, const TemplateRegistry &Registry,
                SmallVectorImpl<FoundUsage> &Out) {
  scanUsagesImpl(Code, Registry, Out, /*ScanHeaders=*/true);
}

} // namespace c_autotmpl
