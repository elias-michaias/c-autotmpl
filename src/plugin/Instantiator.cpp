#include "Instantiator.h"
#include "SourceScan.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
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

size_t paramCount(const TemplateDef &Template) {
  if (Template.Params.empty())
    return 1;
  return Template.Params.size();
}

static void appendArgDefines(std::string &Out, const TemplateDef &Template,
                             ArrayRef<std::string> Args) {
  size_t Count = paramCount(Template);
  for (size_t I = 0; I < Count && I < Args.size(); ++I) {
    Out += "#define ";
    Out += argumentMacroName(Template, I).str();
    Out += ' ';
    Out += Args[I];
    Out += '\n';
  }
}

static void appendArgUndefs(std::string &Out, const TemplateDef &Template) {
  size_t Count = paramCount(Template);
  for (size_t I = Count; I > 0; --I) {
    Out += "#undef ";
    Out += argumentMacroName(Template, I - 1).str();
    Out += '\n';
  }
}

static void appendStcDefines(std::string &Out, const TemplateDef &Template,
                             ArrayRef<std::string> Args) {
  if (!usesDerivedStcNames(Template)) {
    appendArgDefines(Out, Template, Args);
    return;
  }
  Out += "#define i_type ";
  Out += stcContainerName(Template, Args);
  Out += '\n';
  appendArgDefines(Out, Template, Args);
  std::string Traits = stcTraits(Template, Args);
  if (!Traits.empty()) {
    Out += "#define i_traits ";
    Out += Traits;
    Out += '\n';
  }
}

static void appendStcUndefs(std::string &Out, const TemplateDef &Template,
                            ArrayRef<std::string> Args) {
  if (!usesDerivedStcNames(Template)) {
    appendArgUndefs(Out, Template);
    return;
  }
  if (!stcTraits(Template, Args).empty())
    Out += "#undef i_traits\n";
  appendArgUndefs(Out, Template);
  Out += "#undef i_type\n";
}

/// Derived STC wrappers expose type templates such as vec(int). Each adapter
/// macro sampled from a call site adds the matching function template, such as
/// vec_push(int)(&xs, 7). Operation names come from source, not from a baked-in
/// STC version list.
static void appendStcAdapterCat(std::string &Out) {
  Out += "#ifndef C_AUTOTMPL_STC_ADAPTER_CAT\n";
  Out += "#define C_AUTOTMPL_STC_ADAPTER_CAT_I(A, B) A##B\n";
  Out += "#define C_AUTOTMPL_STC_ADAPTER_CAT(A, B) "
         "C_AUTOTMPL_STC_ADAPTER_CAT_I(A, B)\n";
  Out += "#endif\n";
}

static void appendStcAdapterMacro(std::string &Out, const TemplateDef &Template,
                                  StringRef Op, bool Guarded) {
  if (Guarded) {
    Out += "#ifndef C_AUTOTMPL_STC_ADAPTER_";
    Out += Template.Name;
    Out += '_';
    Out += Op;
    Out += '\n';
    Out += "#define C_AUTOTMPL_STC_ADAPTER_";
    Out += Template.Name;
    Out += '_';
    Out += Op;
    Out += '\n';
  }
  Out += "#define ";
  Out += Template.Name;
  Out += '_';
  Out += Op;
  Out += "(...) C_AUTOTMPL_STC_ADAPTER_CAT(";
  Out += Template.Name;
  Out += "(__VA_ARGS__), _";
  Out += Op;
  Out += ")\n";
  if (Guarded)
    Out += "#endif\n";
}

static void appendStcAdapters(std::string &Out, const TemplateDef &Template,
                              ArrayRef<StringRef> Ops) {
  if (!usesDerivedStcNames(Template) || Ops.empty())
    return;
  appendStcAdapterCat(Out);
  for (StringRef Op : Ops)
    appendStcAdapterMacro(Out, Template, Op, /*Guarded=*/false);
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

static bool fileDefinesMacro(StringRef Content, StringRef Macro) {
  for (const LineInfo &Info : splitLines(Content)) {
    StringRef S = Info.Text.ltrim();
    if (!S.consume_front("#define"))
      continue;
    S = S.ltrim();
    size_t End = 0;
    while (End < S.size() &&
           (S[End] == '_' || (S[End] >= 'a' && S[End] <= 'z') ||
            (S[End] >= 'A' && S[End] <= 'Z') ||
            (S[End] >= '0' && S[End] <= '9')))
      ++End;
    if (S.take_front(End) == Macro)
      return true;
  }
  return false;
}

/// Finds one instantiation block at Lines[Index], marked or hand-written, and
/// reports its byte range. Trailing blank lines are included so removal does
/// not leave extra whitespace behind.
bool findBlockAt(ArrayRef<LineInfo> Lines, size_t Index,
                 const TemplateDef &Template, std::string &Type, size_t &Begin,
                 size_t &End, bool &Marked) {
  if (Index >= Lines.size())
    return false;

  size_t Count = paramCount(Template);
  StringRef First = Lines[Index].Text.trim();
  SmallVector<std::string, 4> ParsedArgs;

  if (First.starts_with(MarkerPrefix) && First.ends_with(MarkerSuffix)) {
    StringRef Inner =
        First.drop_front(MarkerPrefix.size()).drop_back(MarkerSuffix.size());
    size_t Open = Inner.find('(');
    size_t Close = Inner.rfind(')');
    if (Open == StringRef::npos || Close == StringRef::npos || Close < Open)
      return false;
    if (Inner.take_front(Open).trim() != Template.Name)
      return false;
    Type = Inner.slice(Open + 1, Close).trim().str();
    Marked = true;
  } else {
    StringRef FirstArg;
    if (!parseDefine(First, argumentMacroName(Template, 0), FirstArg))
      return false;
    ParsedArgs.resize(Count);
    ParsedArgs[0] = FirstArg.str();
    Marked = false;
  }
  if (!Marked && ParsedArgs.empty())
    return false;

  bool SawInclude = false;
  bool SawConditional = false;
  int Depth = 0;
  size_t UndefIndex = StringRef::npos;
  size_t LastLine = StringRef::npos;
  // Generated STC adapters add a block of function-template macros, so the
  // include and undef lines can be well past the old 25-line window.
  size_t Last = std::min(Index + 80 + 5 * Count, Lines.size());
  SmallVector<bool, 4> SeenUndef(Count, false);
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
    if (lineIncludes(Line, Template.IncludeFromGenDir) ||
        (!Template.IncludeFromImplDir.empty() &&
         lineIncludes(Line, Template.IncludeFromImplDir)))
      SawInclude = true;
    bool AllUndef = true;
    for (size_t J = 0; J < Count; ++J) {
      StringRef Name = argumentMacroName(Template, J);
      if (lineUndefines(Line, Name))
        SeenUndef[J] = true;
      if (!SeenUndef[J])
        AllUndef = false;
      if (!Marked && !Name.empty()) {
        StringRef Arg;
        if (parseDefine(Line, Name, Arg) && J < ParsedArgs.size())
          ParsedArgs[J] = Arg.str();
      }
    }
    if (AllUndef) {
      UndefIndex = I;
      // Unguarded blocks end at their last #undef. Guarded blocks keep
      // scanning to the matching #endif so nested presence/guard pairs are
      // handled.
      if (!SawConditional)
        break;
    }
  }
  if (!SawInclude || UndefIndex == StringRef::npos)
    return false;

  if (!Marked) {
    for (const std::string &Arg : ParsedArgs)
      if (Arg.empty())
        return false;
    Type = joinExplicitArgs(ParsedArgs);
  }
  if (Type.empty())
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
    std::string Type;
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
///
/// The name is readable and collision-free: letters and digits stay as written,
/// while other bytes use _HH. A literal underscore followed by two hex digits
/// is escaped as _5F so it cannot be mistaken for an escape sequence.
static bool isMacroHexDigit(unsigned char C) {
  return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f') ||
         (C >= 'A' && C <= 'F');
}

static std::string presenceMacroWithPrefix(StringRef Prefix,
                                           StringRef TemplateName,
                                           StringRef TypeSpelling) {
  std::string Name = TemplateName.str() + "_" + TypeSpelling.str();
  static const char Hex[] = "0123456789ABCDEF";
  std::string Out = Prefix.str();
  for (size_t I = 0; I < Name.size(); ++I) {
    unsigned char C = static_cast<unsigned char>(Name[I]);
    if ((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
        (C >= '0' && C <= '9')) {
      Out += static_cast<char>(C);
    } else if (C == '_' && I + 2 < Name.size() &&
               isMacroHexDigit(static_cast<unsigned char>(Name[I + 1])) &&
               isMacroHexDigit(static_cast<unsigned char>(Name[I + 2]))) {
      Out += "_5F";
    } else if (C == '_') {
      Out += '_';
    } else {
      Out += '_';
      Out += Hex[C >> 4];
      Out += Hex[C & 0xF];
    }
  }
  return Out;
}

std::string presenceMacro(StringRef TemplateName, StringRef TypeSpelling) {
  return presenceMacroWithPrefix("C_AUTOTMPL_", TemplateName, TypeSpelling);
}

std::string implPresenceMacro(StringRef TemplateName,
                              StringRef TypeSpelling) {
  return presenceMacroWithPrefix("C_AUTOTMPL_IMPL_", TemplateName,
                                 TypeSpelling);
}

std::string makeInstantiationBlock(
    const TemplateDef &Template, StringRef TypeSpelling, StringRef GuardMacro,
    ArrayRef<DependencyGuard> ExtraGuards, ArrayRef<StringRef> AdapterOps) {
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

  SmallVector<std::string, 4> Args;
  splitExplicitArgs(TypeSpelling, Template, Args);
  appendStcDefines(Out, Template, Args);
  appendStcAdapters(Out, Template, AdapterOps);

  bool SharedStc = Template.Preset == TemplatePreset::StcShared;
  if (Template.HasTmplHeaderGuard && !isStcPreset(Template))
    Out += "#define TMPL_HEADER 1\n";
  if (SharedStc) {
    Out += "#define TMPL_HEADER 1\n";
    Out += "#define i_header 1\n";
  }
  Out += "#include \"";
  Out += Template.IncludeFromGenDir;
  Out += "\"\n";
  if (SharedStc)
    Out += "#undef i_header\n";
  if (Template.HasTmplHeaderGuard && !isStcPreset(Template))
    Out += "#undef TMPL_HEADER\n";
  if (SharedStc)
    Out += "#undef TMPL_HEADER\n";
  appendStcUndefs(Out, Template, Args);
  for (unsigned I = Guards.size(); I > 0; --I)
    Out += "#endif\n";
  Out += "#endif\n";
  Out += '\n';
  return Out;
}

static bool isDispatchIdentChar(char C) {
  return C == '_' || (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
         (C >= '0' && C <= '9');
}

static void appendDispatchArg(std::string &Out, StringRef Raw) {
  static const char Hex[] = "0123456789ABCDEF";
  std::string Arg = Raw.trim().str();
  Out += '_';
  for (char C : Arg) {
    unsigned char Byte = static_cast<unsigned char>(C);
    if (isDispatchIdentChar(C))
      Out += C;
    else {
      Out += '_';
      Out += Hex[Byte >> 4];
      Out += Hex[Byte & 0xF];
    }
  }
}

std::string dispatchContainerName(const TemplateDef &Template,
                                  ArrayRef<std::string> Args) {
  std::string Out = Template.Name;
  for (const std::string &Raw : Args)
    appendDispatchArg(Out, Raw);
  return Out;
}

static std::string dispatchCallTarget(const TemplateDef &Template,
                                      StringRef Op, const DispatchCase &Case) {
  auto It = Template.DispatchTargets.find(Op);
  if (It == Template.DispatchTargets.end())
    return Case.Container + "_" + Op.str();
  const DispatchTargetInfo &Target = It->second;
  if (!Target.Base.empty()) {
    if (!Target.AppendArgs)
      return Target.Base;
    std::string Out = Target.Base;
    for (const std::string &Arg : Case.Args)
      appendDispatchArg(Out, Arg);
    return Out;
  }
  return Case.Container + "_" +
         (Target.Suffix.empty() ? Op.str() : Target.Suffix);
}

static constexpr size_t DispatchMaxArgs = 8;

static std::string dispatchSelectorName(StringRef TemplateName, StringRef Op,
                                        size_t Arity, size_t Index) {
  std::string Out = "C_AUTOTMPL_DISPATCH_SEL_" + TemplateName.str() + "_" +
                    Op.str() + "_" + std::to_string(Arity);
  if (Index)
    Out += "_" + std::to_string(Index);
  return Out;
}

static std::string dispatchParams(size_t Arity) {
  std::string Out;
  for (size_t I = 1; I <= Arity; ++I) {
    if (I > 1)
      Out += ", ";
    Out += "p" + std::to_string(I);
  }
  return Out;
}

static void appendDispatchCaseSelector(std::string &Out,
                                       const DispatchConfig &Config,
                                       StringRef TemplateName, StringRef Op,
                                       size_t CaseIndex, size_t Arity,
                                       StringRef Container, StringRef Target) {
  std::string Name = dispatchSelectorName(TemplateName, Op, Arity, CaseIndex);
  std::string Previous =
      dispatchSelectorName(TemplateName, Op, Arity, CaseIndex - 1);
  std::string Params = dispatchParams(Arity);
  std::string Expr = Previous + "(" + Params + ")";

  for (size_t K = Arity; K > 0; --K) {
    std::string Param = "p" + std::to_string(K);
    std::string Next;
    if (Config.Backend == DispatchBackend::Builtin) {
      Next = "__builtin_choose_expr(__builtin_types_compatible_p(__typeof__(" +
             Param + "), " + Container.str() + "), " + Target.str() +
             ", __builtin_choose_expr(__builtin_types_compatible_p(__typeof__(" +
             Param + "), " + Container.str() + "*), " + Target.str() +
             ", __builtin_choose_expr(__builtin_types_compatible_p(__typeof__(" +
             Param + "), const " + Container.str() + "*), " + Target.str() + ", " +
             Expr + ")))";
    } else {
      Next = "_Generic((" + Param + "), " + Container.str() + ": " + Target.str() +
             ", " + Container.str() + "*: " + Target.str() + ", const " +
             Container.str() + "*: " + Target.str() + ", default: " + Expr +
             ")";
    }
    Expr = std::move(Next);
  }

  Out += "#define " + Name + "(" + Params + ") " + Expr + "\n";
}

std::string makeDispatchSection(const TemplateDef &Template,
                                ArrayRef<DispatchCase> Cases,
                                ArrayRef<StringRef> Ops,
                                const DispatchConfig &Config) {
  if (!Config.Enabled || !Template.DispatchEnabled || Cases.empty() ||
      Ops.empty())
    return {};
  std::set<std::string> UniqueOps;
  for (StringRef Op : Ops)
    if (!Op.empty())
      UniqueOps.insert(Op.str());
  if (UniqueOps.empty())
    return {};

  std::string Out;
  Out += "/* c-autotmpl dispatch begin " + Template.Name + " */\n";
  Out += "#ifndef C_AUTOTMPL_DISPATCH_MISSING\n";
  Out += "#define C_AUTOTMPL_DISPATCH_MISSING\n";
  Out += "static inline int c_autotmpl_dispatch_missing(void) "
         "{ return 0; }\n";
  Out += "#endif\n";
  for (const std::string &Op : UniqueOps) {
    std::string Guard = "C_AUTOTMPL_DISPATCH_" + Template.Name + "_" + Op;
    Out += "#ifndef " + Guard + "\n#define " + Guard + "\n";

    for (size_t Arity = 1; Arity <= DispatchMaxArgs; ++Arity) {
      std::string Base = dispatchSelectorName(Template.Name, Op, Arity, 0);
      std::string Params = dispatchParams(Arity);
      Out += "#define " + Base + "(" + Params +
             ") c_autotmpl_dispatch_missing\n";
      for (size_t I = 0; I < Cases.size(); ++I) {
        std::string Presence = presenceMacro(Template.Name, Cases[I].Type);
        Out += "#ifdef " + Presence + "\n";
        appendDispatchCaseSelector(Out, Config, Template.Name, Op, I + 1,
                                   Arity, Cases[I].Container,
                                   dispatchCallTarget(Template, Op, Cases[I]));
        Out += "#else\n";
        Out += "#define " +
               dispatchSelectorName(Template.Name, Op, Arity, I + 1) + "(" +
               Params + ") " +
               dispatchSelectorName(Template.Name, Op, Arity, I) + "(" +
               Params + ")\n";
        Out += "#endif\n";
      }
    }

    std::string Pick = "C_AUTOTMPL_DISPATCH_PICK_" + Template.Name + "_" + Op;
    Out += "#define " + Pick + "(";
    for (size_t K = 1; K <= DispatchMaxArgs + 1; ++K) {
      if (K > 1)
        Out += ", ";
      Out += "_" + std::to_string(K);
    }
    Out += ", ...) _" + std::to_string(DispatchMaxArgs + 1) + "\n";
    std::string MacroName = Op;
    if (!Config.Truncate) {
      if (Config.Style == DispatchStyle::Prefix)
        MacroName = Config.Affix + MacroName;
      else
        MacroName += Config.Affix;
    }
    Out += "#define " + MacroName + "(...) " + Pick + "(__VA_ARGS__";
    for (size_t K = DispatchMaxArgs; K > 0; --K)
      Out += ", " + dispatchSelectorName(Template.Name, Op, K, Cases.size());
    Out += ", " + dispatchSelectorName(Template.Name, Op, 1, Cases.size()) +
           ")(__VA_ARGS__)(__VA_ARGS__)\n";
    Out += "#endif\n";
  }
  Out += "/* c-autotmpl dispatch end " + Template.Name + " */\n";
  return Out;
}

std::string makeImplInstantiationBlock(const TemplateDef &Template,
                                       StringRef TypeSpelling,
                                       ArrayRef<std::string> Args,
                                       ArrayRef<std::string> GuardMacros) {
  size_t Count = paramCount(Template);
  SmallVector<std::string, 4> ActualArgs;
  if (Args.size() == Count)
    for (const std::string &Arg : Args)
      ActualArgs.push_back(Arg);
  else if (!splitExplicitArgs(TypeSpelling, Template, ActualArgs) ||
           ActualArgs.size() != Count)
    return {};

  std::string Out;
  Out.reserve(128 + Template.IncludeFromImplDir.size() + TypeSpelling.size());
  Out += instantiationMarker(Template.Name, TypeSpelling);
  Out += '\n';

  const std::string Presence = implPresenceMacro(Template.Name, TypeSpelling);
  Out += "#ifndef ";
  Out += Presence;
  Out += '\n';
  for (const std::string &Guard : GuardMacros) {
    if (Guard.empty())
      continue;
    Out += "#ifdef ";
    Out += Guard;
    Out += '\n';
  }
  Out += "#define ";
  Out += Presence;
  Out += '\n';

  appendStcDefines(Out, Template, ActualArgs);
  bool SharedStc = Template.Preset == TemplatePreset::StcShared;
  if (SharedStc)
    Out += "#define i_implement 1\n";
  Out += "#include \"";
  Out += Template.IncludeFromImplDir;
  Out += "\"\n";
  if (SharedStc)
    Out += "#undef i_implement\n";
  appendStcUndefs(Out, Template, ActualArgs);
  for (size_t I = GuardMacros.size(); I > 0; --I) {
    if (!GuardMacros[I - 1].empty())
      Out += "#endif\n";
  }
  Out += "#endif\n\n";
  return Out;
}

namespace {

bool fileHasLine(StringRef Content, StringRef Line) {
  StringRef Wanted = Line.trim();
  for (const LineInfo &Info : splitLines(Content))
    if (Info.Text.trim() == Wanted)
      return true;
  return false;
}

std::string makeImplPrologue(ArrayRef<std::string> IncludeLines) {
  std::string Out = "/* Generated by c-autotmpl. Do not edit. */\n";
  Out += "#undef TMPL_HEADER\n";
  llvm::StringSet<> Seen;
  for (const std::string &Line : IncludeLines) {
    if (Line.empty() || !Seen.insert(Line).second)
      continue;
    Out += Line;
    Out += '\n';
  }
  Out += '\n';
  return Out;
}

bool writeTextFile(StringRef Path, StringRef Content, std::string &Error) {
  SmallString<256> Dir(sys::path::parent_path(Path));
  if (!Dir.empty() && !sys::fs::is_directory(Dir)) {
    std::error_code DCEC = sys::fs::create_directories(Dir);
    if (DCEC) {
      Error = DCEC.message();
      return false;
    }
  }

  std::error_code EC;
  raw_fd_ostream OS(Path, EC, sys::fs::CD_CreateAlways);
  if (EC) {
    Error = EC.message();
    return false;
  }
  OS << Content;
  OS.close();
  if (OS.has_error()) {
    Error = "failed writing " + Path.str();
    return false;
  }
  return true;
}

} // namespace

InstantiationResult Instantiator::instantiateImpl(
    const TemplateDef &Template, StringRef TypeSpelling,
    ArrayRef<std::string> Args, ArrayRef<std::string> GuardMacros,
    ArrayRef<std::string> IncludeLines, std::string &GenPath,
    std::string &Error) {
  if (Template.GeneratedImplSource.empty()) {
    Error = "template " + Template.Name + " has no implementation source";
    return InstantiationResult::Error;
  }
  if (Args.size() != paramCount(Template)) {
    Error = "expected " + std::to_string(paramCount(Template)) +
            " type argument(s) for " + Template.Name;
    return InstantiationResult::Error;
  }

  const std::string Block =
      makeImplInstantiationBlock(Template, TypeSpelling, Args, GuardMacros);
  if (Block.empty()) {
    Error = "cannot build implementation block for " + Template.Name + "(" +
            TypeSpelling.str() + ")";
    return InstantiationResult::Error;
  }

  std::lock_guard<std::mutex> Lock(Mutex);
  GenPath = Template.GeneratedImplSource;
  FileState &State = getState(GenPath);
  const std::string Marker = instantiationMarker(Template.Name, TypeSpelling);
  if (State.Markers.contains(Marker) ||
      hasManualInstantiation(State.Content, Template, TypeSpelling))
    return InstantiationResult::Existing;

  std::string NewContent;
  if (State.Content.empty()) {
    NewContent = makeImplPrologue(IncludeLines);
  } else if (!fileHasLine(State.Content, "#undef TMPL_HEADER")) {
    NewContent = makeImplPrologue(IncludeLines);
    NewContent += State.Content;
  } else {
    llvm::StringSet<> Seen;
    std::string Missing;
    for (const std::string &Line : IncludeLines) {
      if (Line.empty() || !Seen.insert(Line).second)
        continue;
      if (!fileHasLine(State.Content, Line)) {
        Missing += Line;
        Missing += '\n';
      }
    }

    size_t Insert = StringRef::npos;
    for (const LineInfo &Info : splitLines(State.Content))
      if (Info.Text.trim() == "#undef TMPL_HEADER") {
        Insert = Info.End;
        break;
      }

    NewContent = State.Content;
    if (Insert == StringRef::npos) {
      NewContent = makeImplPrologue(IncludeLines);
      NewContent += State.Content;
    } else if (!Missing.empty()) {
      NewContent.insert(Insert, Missing);
    }
  }

  if (!NewContent.empty() && NewContent.back() != '\n')
    NewContent += '\n';
  NewContent += Block;

  if (!writeTextFile(GenPath, NewContent, Error))
    return InstantiationResult::Error;

  State.Content = std::move(NewContent);
  State.Stamp = stampFor(GenPath);
  State.Markers.insert(Marker);
  return InstantiationResult::Added;
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

bool Instantiator::ensureAdapters(const TemplateDef &Template,
                                  ArrayRef<StringRef> Ops,
                                  StringRef OutputHeader, std::string &Error) {
  if (!usesDerivedStcNames(Template) || Ops.empty())
    return false;

  std::lock_guard<std::mutex> Lock(Mutex);
  StringRef Path =
      OutputHeader.empty() ? Template.GeneratedHeader : OutputHeader;
  FileState &State = getState(Path);
  StringRef Content(State.Content);
  if (Content.empty())
    return false;

  std::string Missing;
  if (!fileDefinesMacro(Content, "C_AUTOTMPL_STC_ADAPTER_CAT"))
    appendStcAdapterCat(Missing);
  for (StringRef Op : Ops) {
    std::string Guard = std::string("C_AUTOTMPL_STC_ADAPTER_") +
                        Template.Name + "_" + Op.str();
    if (!fileDefinesMacro(Content, Guard))
      appendStcAdapterMacro(Missing, Template, Op, /*Guarded=*/true);
  }
  if (Missing.empty())
    return false;

  size_t Pos = Content.rfind("#endif");
  size_t Insert = Content.size();
  if (Pos != StringRef::npos) {
    size_t LineStart = Content.rfind('\n', Pos);
    Insert = LineStart == StringRef::npos ? 0 : LineStart + 1;
    if (!Content.slice(Insert, Pos).trim().empty())
      Insert = Content.size();
  }

  std::string NewContent;
  NewContent.reserve(Content.size() + Missing.size());
  NewContent.append(Content.data(), Insert);
  NewContent += Missing;
  NewContent.append(Content.data() + Insert, Content.size() - Insert);

  std::error_code EC;
  SmallString<256> Dir(sys::path::parent_path(Path));
  if (!Dir.empty()) {
    EC = sys::fs::create_directories(Dir);
    if (EC) {
      Error = EC.message();
      return false;
    }
  }
  raw_fd_ostream OS(Path, EC, sys::fs::CD_CreateAlways);
  if (EC) {
    Error = EC.message();
    return false;
  }
  OS << NewContent;
  OS.close();
  if (OS.has_error()) {
    Error = "failed writing " + Path.str();
    return false;
  }

  State.Content = std::move(NewContent);
  State.Stamp = stampFor(Path);
  return true;
}

bool Instantiator::ensureDispatch(const TemplateDef &Template,
                                  ArrayRef<DispatchCase> Cases,
                                  ArrayRef<StringRef> Ops,
                                  const DispatchConfig &Config,
                                  StringRef OutputHeader,
                                  std::string &Error) {
  std::lock_guard<std::mutex> Lock(Mutex);
  StringRef Path =
      OutputHeader.empty() ? Template.GeneratedHeader : OutputHeader;
  FileState &State = getState(Path);
  StringRef Content(State.Content);
  std::string Section = makeDispatchSection(Template, Cases, Ops, Config);
  std::string Begin = "/* c-autotmpl dispatch begin " + Template.Name + " */";
  std::string End = "/* c-autotmpl dispatch end " + Template.Name + " */";
  size_t B = Content.find(Begin);
  size_t E = B == StringRef::npos ? StringRef::npos : Content.find(End, B);

  std::string NewContent;
  if (B != StringRef::npos && E != StringRef::npos) {
    size_t After = Content.find('\n', E);
    After = After == StringRef::npos ? E + End.size() : After + 1;
    NewContent.append(Content.data(), B);
    NewContent += Section;
    NewContent.append(Content.data() + After, Content.size() - After);
  } else {
    if (Section.empty())
      return false;
    if (Content.empty()) {
      NewContent += "#ifndef ";
      NewContent += generatedHeaderFileGuard(Path);
      NewContent += "\n#define ";
      NewContent += generatedHeaderFileGuard(Path);
      NewContent += "\n\n";
      NewContent += Section;
      NewContent += "#endif\n";
    } else {
      size_t Pos = Content.rfind("#endif");
      size_t Insert = Content.size();
      if (Pos != StringRef::npos) {
        size_t LineStart = Content.rfind('\n', Pos);
        Insert = LineStart == StringRef::npos ? 0 : LineStart + 1;
        if (!Content.slice(Insert, Pos).trim().empty())
          Insert = Content.size();
      }
      NewContent.append(Content.data(), Insert);
      NewContent += Section;
      NewContent.append(Content.data() + Insert, Content.size() - Insert);
    }
  }
  if (NewContent == Content.str())
    return false;

  std::error_code EC;
  SmallString<256> Dir(sys::path::parent_path(Path));
  if (!Dir.empty()) {
    EC = sys::fs::create_directories(Dir);
    if (EC) {
      Error = EC.message();
      return false;
    }
  }
  raw_fd_ostream OS(Path, EC, sys::fs::CD_CreateAlways);
  if (EC) {
    Error = EC.message();
    return false;
  }
  OS << NewContent;
  OS.close();
  if (OS.has_error()) {
    Error = "failed writing " + Path.str();
    return false;
  }
  State.Content = std::move(NewContent);
  State.Stamp = stampFor(Path);
  return true;
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
      std::string Type;
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
                          StringRef OutputHeader,
                          ArrayRef<StringRef> AdapterOps) {
  SmallVector<std::string, 4> Args;
  if (!splitExplicitArgs(TypeSpelling, Template, Args) ||
      Args.size() != paramCount(Template)) {
    Error = "expected " + std::to_string(paramCount(Template)) +
            " type argument(s) for " + Template.Name;
    return InstantiationResult::Error;
  }

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
    NewContent += makeInstantiationBlock(Template, TypeSpelling, GuardMacro,
                                         ExtraGuards, AdapterOps);
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
                                           ExtraGuards, AdapterOps);
      NewContent.append(Content.data() + GuardLineStart,
                        Content.size() - GuardLineStart);
    } else {
      NewContent = Content.str();
      NewContent += '\n';
      NewContent += makeInstantiationBlock(Template, TypeSpelling, GuardMacro,
                                           ExtraGuards, AdapterOps);
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
