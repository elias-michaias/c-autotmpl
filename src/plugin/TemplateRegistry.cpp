#include "TemplateRegistry.h"
#include "SourceScan.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
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

size_t matchingParen(StringRef S, size_t Open) {
  int Depth = 0;
  for (size_t I = Open; I < S.size(); ++I) {
    if (S[I] == '(')
      ++Depth;
    else if (S[I] == ')') {
      if (--Depth == 0)
        return I;
    }
  }
  return S.size();
}

size_t declarationBegin(StringRef S, size_t Ident) {
  for (size_t I = Ident; I > 0; --I)
    if (S[I - 1] == ';' || S[I - 1] == '{' || S[I - 1] == '}')
      return I;
  return 0;
}

bool isWord(StringRef S, size_t I, StringRef Word) {
  if (I + Word.size() > S.size() || S.substr(I, Word.size()) != Word)
    return false;
  if (I && isIdentChar(S[I - 1]))
    return false;
  return I + Word.size() == S.size() || !isIdentChar(S[I + Word.size()]);
}

/// A lexical declaration defines a function when it is not a typedef or the
/// closing brace of an aggregate type. This keeps generated type names such as
/// Vec(T) from being mistaken for function declarations.
bool declarationIsFunctional(StringRef S, size_t Begin, size_t End) {
  StringRef Text = S.substr(Begin, End - Begin);
  for (size_t I = 0; I < Text.size();) {
    if (Text[I] == '}')
      return false;
    if (!isIdentStart(Text[I])) {
      ++I;
      continue;
    }
    size_t J = I + 1;
    while (J < Text.size() && isIdentChar(Text[J]))
      ++J;
    if (isWord(Text, I, "typedef"))
      return false;
    I = J;
  }
  return true;
}

/// A declaration must have a return-type prefix before the call-like token and
/// a function tail after it. This rejects struct attributes such as
/// `_Alignas(T)`, whose argument list also mentions a template parameter.
bool declarationHasPrefix(StringRef S, size_t Begin, size_t Ident) {
  for (size_t I = Begin; I < Ident; ++I)
    if (isIdentStart(S[I]))
      return true;
  return false;
}

bool declarationTailIsFunction(StringRef S, size_t Close) {
  size_t I = Close + 1;
  for (;;) {
    while (I < S.size() && isSpace(S[I]))
      ++I;
    if (I >= S.size())
      return false;
    if (S[I] == ';' || S[I] == '{')
      return true;
    if (!isIdentStart(S[I]))
      return false;
    while (I < S.size() && isIdentChar(S[I]))
      ++I;
    while (I < S.size() && isSpace(S[I]))
      ++I;
    if (I >= S.size() || S[I] != '(')
      return false;
    size_t End = matchingParen(S, I);
    if (End >= S.size())
      return false;
    I = End + 1;
  }
}

/// Eligibility depends only on the function's parameter list. A parameter that
/// mentions the template call spelling, a generated decorator parameter, or a
/// decorator name such as Key makes the function eligible. Pointer-ness and
/// argument position are irrelevant.
bool paramsUseTemplate(StringRef S, size_t Begin, size_t End,
                       const TemplateDef &Template) {
  std::string Params = S.substr(Begin, End - Begin).trim().str();
  std::string Call = Template.Name + "(";
  std::string Mangled = Template.Name + "_";
  if (Params.find(Call) != std::string::npos ||
      Params.find(Mangled) != std::string::npos)
    return true;
  for (size_t I = 0; I < Params.size();) {
    if (!isIdentStart(Params[I])) {
      ++I;
      continue;
    }
    size_t J = I + 1;
    while (J < Params.size() && isIdentChar(Params[J]))
      ++J;
    StringRef Token(Params.data() + I, J - I);
    if (Token == "i_type")
      return true;
    for (size_t K = 0; K < Template.Params.size(); ++K)
      if (Token == parameterName(Template, K) ||
          Token == argumentMacroName(Template, K))
        return true;
    I = J;
  }
  return false;
}

/// Distinguishes a parameter declaration from a bare type used by a generated
/// type name. T value and Box(T) *b qualify; T and Vec(T) alone do not.
bool paramsDeclareArgument(StringRef S, size_t Begin, size_t End) {
  std::string Params = S.substr(Begin, End - Begin).trim().str();
  int Depth = 0;
  size_t Part = 0;
  for (size_t I = 0; I <= Params.size(); ++I) {
    char C = I < Params.size() ? Params[I] : '\0';
    if (C == '(' || C == '[')
      ++Depth;
    else if (C == ')' || C == ']')
      --Depth;
    if (C && !(C == ',' && Depth == 0))
      continue;
    StringRef Arg(Params.data() + Part, I - Part);
    Arg = Arg.trim();
    int Identifiers = 0;
    for (size_t K = 0; K < Arg.size();) {
      if (!isIdentStart(Arg[K])) {
        ++K;
        continue;
      }
      ++Identifiers;
      while (K < Arg.size() && isIdentChar(Arg[K]))
        ++K;
    }
    if (Identifiers < 2 && Arg.find('*') == StringRef::npos &&
        Arg.find('[') == StringRef::npos)
      return false;
    Part = I + 1;
  }
  return true;
}

bool DispatchTruncate = false;

/// In truncate mode `<op>_impl(T, U)` is the canonical generic-method spelling.
/// The suffix marks the declaration macro; the public operation is the part
/// before it, and the concrete target is `<op>_<args>`.
std::string implOpName(StringRef Ident) {
  StringRef Suffix = "_impl";
  if (Ident.size() <= Suffix.size() || !Ident.ends_with(Suffix))
    return {};
  return Ident.drop_back(Suffix.size()).str();
}

/// `BOX_FN(box_set, T)(Box(T) *, T)` or
/// `PAIR_FN(pair_set, T, U)(Pair(T, U) *, T, U)` declares a concrete function
/// whose name is pasted from the first macro argument and `_` plus the template
/// arguments. The first argument is the public op and becomes the
/// `base_<args>` target base. Nested paste macros such as `BOX_CAT(_, T)` in
/// the remaining arguments are fine.
std::string directMangledOpName(StringRef Args, const TemplateDef &Template) {
  size_t FirstEnd = Args.size();
  int Depth = 0;
  for (size_t I = 0; I < Args.size(); ++I) {
    char C = Args[I];
    if (C == '(' || C == '[')
      ++Depth;
    else if (C == ')' || C == ']')
      --Depth;
    else if (C == ',' && Depth == 0) {
      FirstEnd = I;
      break;
    }
  }
  if (FirstEnd == Args.size())
    return {};
  StringRef First = Args.take_front(FirstEnd).trim();
  if (First.empty() || !isIdentStart(First.front()))
    return {};
  for (char C : First)
    if (!isIdentChar(C))
      return {};

  bool HasParam = false;
  StringRef Rest = Args.drop_front(FirstEnd + 1);
  for (size_t I = 0; I < Rest.size() && !HasParam;) {
    if (!isIdentStart(Rest[I])) {
      ++I;
      continue;
    }
    size_t Start = I++;
    while (I < Rest.size() && isIdentChar(Rest[I]))
      ++I;
    StringRef Token = Rest.slice(Start, I);
    if (Token == "i_type")
      HasParam = true;
    for (size_t K = 0; K < Template.Params.size() && !HasParam; ++K)
      if (Token == parameterName(Template, K))
        HasParam = true;
  }
  return HasParam ? First.str() : std::string();
}

static std::string opForSuffix(StringRef Ident, StringRef Suffix) {
  if (Suffix.empty() || Ident.size() <= Suffix.size() ||
      !Ident.ends_with(Suffix))
    return {};
  return Ident.drop_back(Suffix.size()).str();
}

/// `box_set_int(Box_int *b, int v)` registers the bare op `box_set` in truncate
/// mode. Both a mangled parameter token (`Box_int`) and a template call
/// (`pair(int, float)`) identify the `_<args>` suffix to drop.
std::string truncatedOpName(StringRef Ident, StringRef Params,
                            const TemplateDef &Template) {
  std::string Prefix = Template.Name + "_";
  for (size_t I = 0; I < Params.size();) {
    if (!isIdentStart(Params[I])) {
      ++I;
      continue;
    }
    size_t Start = I++;
    while (I < Params.size() && isIdentChar(Params[I]))
      ++I;
    StringRef Token = Params.slice(Start, I);
    if (Token.size() <= Prefix.size() ||
        !Token.starts_with(StringRef(Prefix)))
      continue;
    std::string Op =
        opForSuffix(Ident, ("_" + Token.drop_front(Prefix.size()).str()));
    if (!Op.empty())
      return Op;
  }

  StringRef Name(Template.Name);
  for (size_t I = 0; I + Name.size() <= Params.size(); ++I) {
    if (!Params.substr(I).starts_with(Name))
      continue;
    if ((I && isIdentChar(Params[I - 1])) ||
        (I + Name.size() < Params.size() &&
         isIdentChar(Params[I + Name.size()])))
      continue;
    size_t J = I + Name.size();
    while (J < Params.size() && isSpace(Params[J]))
      ++J;
    if (J >= Params.size() || Params[J] != '(')
      continue;
    size_t Close = matchingParen(Params, J);
    if (Close >= Params.size())
      continue;

    std::string Suffix;
    size_t Part = J + 1;
    int Depth = 1;
    for (size_t K = Part; K <= Params.size(); ++K) {
      char C = K < Params.size() ? Params[K] : '\0';
      bool Boundary = K == Params.size() || K == Close;
      if (!Boundary) {
        if (C == '(' || C == '[')
          ++Depth;
        else if (C == ')' || C == ']')
          --Depth;
        else if (C == ',' && Depth == 1)
          Boundary = true;
      }
      if (!Boundary)
        continue;
      StringRef Arg = Params.slice(Part, K).trim();
      size_t Last = Arg.size();
      while (Last && !isIdentChar(Arg[Last - 1]))
        --Last;
      size_t First = Last;
      while (First && isIdentChar(Arg[First - 1]))
        --First;
      if (Last > First) {
        Suffix += '_';
        Suffix += Arg.slice(First, Last).str();
      }
      Part = K + 1;
      if (K == Close)
        break;
    }
    std::string Op = opForSuffix(Ident, Suffix);
    if (!Op.empty())
      return Op;
  }
  return {};
}

/// Finds functions generated by a type-template header. Eligibility depends
/// only on the parameter list; function names are never compared with the
/// template name. A paste-macro declaration such as VEC_FN(i_type, _push)(...)
/// contributes `<Template>_push`, while box_alloc(T)(T value) and
/// inspect(Box(T) *) keep their own spelling. Declarations after
/// `#ifndef TMPL_HEADER` are implementation-only and are ignored.
void collectDispatchOps(StringRef Content, TemplateDef &Out,
                        size_t BaseOffset = 0) {
  if (Out.Kind != TemplateKind::Type || Out.Name.empty())
    return;
  std::string Cleaned = stripCommentsAndLiterals(Content);
  StringRef Clean(Cleaned);
  auto recordOp = [&](StringRef Op, size_t Offset, StringRef Base,
                      StringRef Suffix, bool AppendArgs) {
    std::string Key = Op.str();
    Out.DispatchOps.insert(Key);
    Out.DispatchOpOffsets.try_emplace(
        Key, static_cast<unsigned>(BaseOffset + Offset));
    DispatchTargetInfo &Target = Out.DispatchTargets[Key];
    Target.Base = Base.str();
    Target.Suffix = Suffix.str();
    Target.AppendArgs = AppendArgs;
  };
  bool LineStart = true;
  for (size_t I = 0; I < Clean.size();) {
    size_t LinePos = I;
    while (LinePos < Clean.size() &&
           (Clean[LinePos] == ' ' || Clean[LinePos] == '\t'))
      ++LinePos;
    if (LineStart && LinePos < Clean.size() && Clean[LinePos] == '#') {
      size_t DirectiveEnd = Clean.find('\n', I);
      if (DirectiveEnd == StringRef::npos)
        DirectiveEnd = Clean.size();
      StringRef Directive = Clean.substr(I, DirectiveEnd - I);
      if (Directive.contains("TMPL_HEADER") &&
          (Directive.contains("#ifndef") || Directive.contains("#if !defined")))
        break;
      bool Continued;
      do {
        size_t End = DirectiveEnd;
        Continued = End > I && Clean[End - 1] == '\\';
        I = End < Clean.size() ? End + 1 : End;
        if (!Continued)
          break;
        DirectiveEnd = Clean.find('\n', I);
        if (DirectiveEnd == StringRef::npos)
          DirectiveEnd = Clean.size();
      } while (I < Clean.size());
      LineStart = true;
      continue;
    }
    if (!isIdentStart(Clean[I])) {
      LineStart = Clean[I] == '\n';
      ++I;
      continue;
    }
    LineStart = false;
    size_t End = I + 1;
    while (End < Clean.size() && isIdentChar(Clean[End]))
      ++End;
    StringRef Ident = Clean.slice(I, End);
    size_t After = End;
    while (After < Clean.size() && isSpace(Clean[After]))
      ++After;
    if (After < Clean.size() && Clean[After] == '(') {
      size_t Close = matchingParen(Clean, After);
      if (Close < Clean.size()) {
        size_t DeclBegin = declarationBegin(Clean, I);
        bool Functional = declarationIsFunctional(Clean, DeclBegin, I);
        size_t AfterMacro = Close + 1;
        while (AfterMacro < Clean.size() && isSpace(Clean[AfterMacro]))
          ++AfterMacro;
        if (AfterMacro < Clean.size() && Clean[AfterMacro] == '(') {
          size_t CallClose = matchingParen(Clean, AfterMacro);
          if (CallClose < Clean.size()) {
            size_t LastBegin = After + 1;
            int Depth = 0;
            for (size_t K = After + 1; K < Close; ++K) {
              if (Clean[K] == '(' || Clean[K] == '[')
                ++Depth;
              else if (Clean[K] == ')' || Clean[K] == ']')
                --Depth;
              else if (Clean[K] == ',' && Depth == 0)
                LastBegin = K + 1;
            }
            StringRef Last = Clean.substr(LastBegin, Close - LastBegin).trim();
            bool PasteOp = Last.starts_with("_") && Last.size() > 1 &&
                           isIdentStart(Last[1]);
            for (size_t K = 1; PasteOp && K < Last.size(); ++K)
              if (!isIdentChar(Last[K]))
                PasteOp = false;
            bool MacroUses =
                paramsUseTemplate(Clean, After + 1, Close, Out);
            bool ArgsUse =
                CallClose > AfterMacro + 1 &&
                paramsUseTemplate(Clean, AfterMacro + 1, CallClose, Out) &&
                paramsDeclareArgument(Clean, AfterMacro + 1, CallClose);
            if (Functional && declarationHasPrefix(Clean, DeclBegin, I) &&
                declarationTailIsFunction(Clean, CallClose) &&
                (ArgsUse || MacroUses)) {
              if (DispatchTruncate) {
                std::string Direct = implOpName(Ident);
                if (Direct.empty())
                  Direct = directMangledOpName(
                      Clean.slice(After + 1, Close), Out);
                if (!Direct.empty()) {
                  recordOp(Direct, I, Direct, "", true);
                } else if (PasteOp) {
                  std::string PublicOp =
                      (Out.Name + "_" + Last.drop_front()).str();
                  recordOp(PublicOp, I, "", Last.drop_front(), false);
                } else {
                  recordOp(Ident, I, Ident, "", true);
                }
              } else if (PasteOp) {
                std::string PublicOp = (Out.Name + "_" + Last.drop_front()).str();
                recordOp(PublicOp, I, "", Last.drop_front(), false);
              } else {
                recordOp(Ident, I, Ident, "", true);
              }
            }
          }
        } else if (Functional && Close > After + 1 &&
                   declarationHasPrefix(Clean, DeclBegin, I) &&
                   declarationTailIsFunction(Clean, Close) &&
                   paramsUseTemplate(Clean, After + 1, Close, Out) &&
                   paramsDeclareArgument(Clean, After + 1, Close)) {
          if (DispatchTruncate) {
            std::string Op = truncatedOpName(
                Ident, Clean.slice(After + 1, Close), Out);
            if (!Op.empty()) {
              recordOp(Op, I, Op, "", true);
            } else {
              recordOp(Ident, I, Ident, "", false);
            }
          } else {
            recordOp(Ident, I, Ident, "", false);
          }
        }
      }
    }
    I = End;
  }
}

bool parseDispatchBool(StringRef Text, bool &Out) {
  Text = Text.trim();
  if (Text == "1" || Text.equals_insensitive("true") ||
      Text.equals_insensitive("yes") || Text.equals_insensitive("on")) {
    Out = true;
    return true;
  }
  if (Text == "0" || Text.equals_insensitive("false") ||
      Text.equals_insensitive("no") || Text.equals_insensitive("off")) {
    Out = false;
    return true;
  }
  return false;
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

void setDispatchTruncate(bool Value) { DispatchTruncate = Value; }

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
    std::string FullText = (*BufferOrErr)->getBuffer().str();
    StringRef Full(FullText);
    StringRef Cursor(FullText);
    bool HasGuard = hasTmplHeaderGuard(Full);

    auto Def = std::make_unique<TemplateDef>();
    bool Found = false;
    while (!Cursor.empty()) {
      StringRef Line;
      std::tie(Line, Cursor) = Cursor.split('\n');
      if (parseDecorator(Line, *Def)) {
        Found = true;
        break;
      }
    }
    if (!Found)
      continue;
    collectDispatchOps(Full, *Def);

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

namespace {

StringMap<std::string> &configValues() {
  static StringMap<std::string> Values;
  return Values;
}

bool &configLoaded() {
  static bool Loaded = false;
  return Loaded;
}

std::string lowerKey(StringRef Key) {
  std::string Out;
  for (char C : Key)
    Out.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(C))));
  return Out;
}

void addConfigLine(StringRef Line, StringMap<std::string> &Values) {
  Line = Line.trim();
  if (Line.empty() || Line[0] == '#')
    return;
  size_t Eq = Line.find('=');
  if (Eq == StringRef::npos)
    return;
  StringRef Key = Line.substr(0, Eq).trim();
  StringRef Value = Line.substr(Eq + 1).trim();
  if (Value.size() >= 2 &&
      ((Value[0] == '"' && Value[Value.size() - 1] == '"') ||
       (Value[0] == '\'' && Value[Value.size() - 1] == '\'')))
    Value = Value.substr(1, Value.size() - 2);
  if (!Key.empty())
    Values[lowerKey(Key)] = Value.str();
}

bool findConfigPath(StringRef Start, SmallString<256> &Out) {
  SmallString<256> Dir(Start);
  for (int I = 0; I < 12 && !Dir.empty(); ++I) {
    SmallString<256> Candidate(Dir);
    sys::path::append(Candidate, "autotmpl.conf");
    if (sys::fs::exists(Candidate)) {
      Out = Candidate;
      return true;
    }
    SmallString<256> Parent(sys::path::parent_path(Dir));
    if (Parent == Dir)
      break;
    Dir = Parent;
  }
  return false;
}

} // namespace

void loadConfig(StringRef Root) {
  if (configLoaded())
    return;
  configLoaded() = true;
  SmallString<256> Path;
  if (!findConfigPath(Root, Path))
    return;
  auto Buffer = MemoryBuffer::getFile(Path, /*IsText=*/true);
  if (!Buffer)
    return;
  SmallVector<StringRef, 16> Lines;
  (*Buffer)->getBuffer().split(Lines, '\n');
  for (StringRef Line : Lines)
    addConfigLine(Line, configValues());
}

std::optional<std::string> configValue(StringRef Key) {
  std::string Normalized = lowerKey(Key);
  auto It = configValues().find(Normalized);
  if (It != configValues().end())
    return It->second;

  std::string EnvName = "C_AUTOTMPL_";
  for (char C : Normalized)
    EnvName.push_back(
        static_cast<char>(std::toupper(static_cast<unsigned char>(C))));
  if (auto Env = sys::Process::GetEnv(EnvName))
    return std::string(*Env);
  return std::nullopt;
}

DispatchConfig resolveDispatch() {
  DispatchConfig Config;
  if (auto Value = configValue("dispatch"))
    parseDispatchBool(*Value, Config.Enabled);
  if (auto Value = configValue("dispatch_backend")) {
    if (StringRef(*Value).trim() == "builtin")
      Config.Backend = DispatchBackend::Builtin;
  }
  auto Style = configValue("dispatch_style");
  auto Affix = configValue("dispatch_affix");
  Config.Style = Style && !Style->empty()
                     ? (StringRef(*Style).trim() == "postfix" ||
                                StringRef(*Style).trim() == "post"
                            ? DispatchStyle::Postfix
                            : DispatchStyle::Prefix)
                     : DispatchStyle::Prefix;
  StringRef AffixText = Affix ? StringRef(*Affix).trim() : StringRef();
  if (!AffixText.empty() && AffixText.find_first_of(" \t\r\n,()#") ==
                                StringRef::npos)
    Config.Affix = AffixText.str();
  if (auto Value = configValue("dispatch_truncate"))
    parseDispatchBool(*Value, Config.Truncate);
  if (auto Value = configValue("dispatch_templates")) {
    SmallVector<StringRef, 4> Names;
    StringRef(*Value).split(Names, ',');
    for (StringRef Name : Names) {
      Name = Name.trim();
      if (!Name.empty())
        Config.Templates.push_back(Name.str());
    }
  }
  return Config;
}

void TemplateRegistry::applyDispatch(const DispatchConfig &Config) {
  for (auto &OwnedDef : Owned) {
    TemplateDef &Def = *OwnedDef;
    bool Selected = Config.Enabled && Def.Kind == TemplateKind::Type &&
                    Config.Templates.empty();
    for (const std::string &Name : Config.Templates)
      if (Name == Def.Name)
        Selected = Config.Enabled && Def.Kind == TemplateKind::Type;
    Def.DispatchEnabled = Selected;
  }
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
