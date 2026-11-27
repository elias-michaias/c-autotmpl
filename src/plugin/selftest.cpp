// Small stand-alone check for template discovery and generated-header emission.
// Build: g++ -std=c++17 -Isrc/plugin $(llvm-config --cxxflags) \
//          src/plugin/selftest.cpp src/plugin/TemplateRegistry.cpp \
//          src/plugin/Instantiator.cpp -o /tmp/c-autotmpl-selftest
#include "Instantiator.h"
#include "SourceScan.h"
#include "TemplateRegistry.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace llvm;

namespace {
std::string join(StringRef Base, StringRef Tail) {
  SmallString<128> P(Base);
  sys::path::append(P, Tail);
  return std::string(P);
}

size_t countOccurrences(StringRef Text, StringRef Needle) {
  if (Needle.empty())
    return 0;
  size_t Count = 0;
  for (size_t Pos = Text.find(Needle); Pos != StringRef::npos;
       Pos = Text.find(Needle, Pos + Needle.size()))
    ++Count;
  return Count;
}

std::string readText(StringRef Path) {
  auto Buffer = MemoryBuffer::getFile(Path, /*IsText=*/true);
  assert(Buffer);
  return (*Buffer)->getBuffer().str();
}
} // namespace

int main() {
  SmallString<128> Root("/tmp/c-autotmpl-selftest");
  sys::fs::remove_directories(Root);
  sys::fs::create_directories(join(Root, "tmpl"));
  sys::fs::create_directories(join(Root, "other"));

  assert(c_autotmpl::isNumericLiteral("4"));
  assert(c_autotmpl::isNumericLiteral("0x10u"));
  assert(c_autotmpl::isNumericLiteral("-2.5f"));
  assert(!c_autotmpl::isNumericLiteral("Four"));
  assert(!c_autotmpl::needsIncludeGuard("4"));
  assert(!c_autotmpl::needsIncludeGuard("2.5f"));
  assert(c_autotmpl::needsIncludeGuard("Foo"));

  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "tmpl/list.h"), EC);
    OS << "// -- template List(T)\n#define List(T) List_##T\n";
  }
  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "tmpl/map.h"), EC);
    OS << "// -- template Map(Elem)\n#define Map(Elem) Map_##Elem\n";
  }
  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "tmpl/box.h"), EC);
    OS << "// -- template type Box(T)\n#define Box(T) Box_##T\n";
  }
  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "tmpl/dispatch.h"), EC);
    OS << "// -- template type Dispatch(T)\n"
          "#define Dispatch(T) Dispatch_##T\n"
          "#define DISPATCH_FN(A, B) A##B\n"
          "typedef struct { T value; } Dispatch(T);\n"
          "static inline void Dispatch_int_push(Dispatch(int) *d, int x) "
          "{ d->value = x; }\n"
          "static inline void Dispatch_int_set(int x, Dispatch(int) d) "
          "{ d.value = x; }\n"
          "static inline void DISPATCH_FN(Dispatch(T), _clear)(Dispatch(T) *d) "
          "{ d->value = 0; }\n"
          "static inline int observe(Dispatch(T) *d) { return d->value; }\n"
          "static inline int helper(int x) { return x; }\n";
  }
  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "tmpl/lower.h"), EC);
    OS << "// -- template type Crate(T)\n"
          "#define Crate(T) Crate_##T\n"
          "#define crate_alloc(T) crate_alloc_##T\n"
          "#define crate_free(T) crate_free_##T\n"
          "#define CratePad(T) CratePad_##T\n"
          "typedef struct { T value; } Crate(T);\n"
          "typedef struct { _Alignas(T) char pad[sizeof(T)]; } CratePad(T);\n"
          "static inline Crate(T) *crate_alloc(T)(T value) {\n"
          "  static Crate(T) slot;\n"
          "  slot.value = value;\n"
          "  return &slot;\n"
          "}\n"
          "static inline void crate_free(T)(Crate(T) *c) { c->value = 0; }\n"
          "static inline int observe(Crate(T) *c) { return c->value; }\n"
          "#ifndef TMPL_HEADER\n"
          "#define crate_secret(T) crate_secret_##T\n"
          "static inline int crate_secret(T)(Crate(T) *c) { return c->value; }\n"
          "#endif\n";
  }
  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "tmpl/truncate.h"), EC);
    OS << "// -- template type Direct(T, U)\n"
          "#define DIRECT_CAT4_I(A, B, C, D) A##B##C##D\n"
          "#define DIRECT_CAT4(A, B, C, D) DIRECT_CAT4_I(A, B, C, D)\n"
          "#define DIRECT_CAT5_I(A, B, C, D, E) A##B##C##D##E\n"
          "#define DIRECT_CAT5(A, B, C, D, E) DIRECT_CAT5_I(A, B, C, D, E)\n"
          "#define Direct(T, U) DIRECT_CAT4(Direct_, T, _, U)\n"
          "#define direct_set_impl(T, U) DIRECT_CAT5(direct_set, _, T, _, U)\n"
          "typedef struct { T first; U second; } Direct(T, U);\n"
          "static inline void direct_set_impl(T, U)(Direct(T, U) *d, "
          "T a, U b) { d->first = a; d->second = b; }\n";
  }
  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "tmpl/slab.h"), EC);
    OS << "// -- template type Slab(T)\n"
          "#define Slab(T) Slab_##T\n"
          "typedef struct { T value; } Slab(T);\n"
          "static inline void slab_set_int(Slab(int) *s, int v) "
          "{ s->value = v; }\n";
  }
  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "tmpl/select.h"), EC);
    OS << "// -- template function select(First, Second)\n"
          "#define select(First, Second) select_##First##Second\n";
  }
  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "tmpl/sum.h"), EC);
    OS << "// -- template function sum(Item)\n"
          "static inline Item sum(Item)(List(Item) *xs);\n";
  }
  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "tmpl/option.h"), EC);
    OS << "// -- template type Option(T)\n#define Option(T) Option_##T\n";
  }
  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "tmpl/user.h"), EC);
    OS << "// -- template type User(F, Payload)\n"
          "#define User(F, Payload) User_##F\n"
          "typedef struct { F(Payload) email; } User(F, Payload);\n";
  }
  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "tmpl/wrap.h"), EC);
    OS << "// -- template type Wrap(F)\n"
          "#define Wrap(F) Wrap_##F\n"
          "typedef struct { F(int) value; } Wrap(F);\n";
  }
  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "tmpl/split.h"), EC);
    OS << "// -- template Split(T)\n"
          "/*\n"
          " #ifndef TMPL_HEADER\n"
          "*/\n"
          "#ifndef TMPL_HEADER\n"
          "T split(T)(Split(T) *p);\n"
          "#else\n"
          "typedef struct { T value; } Split(T);\n"
          "#endif\n";
  }
  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "tmpl/stc_vec.h"), EC);
    OS << "// -- template stc_vec(T)\n"
          "#define stc_vec_CAT_I(A, B) A##B\n"
          "#define stc_vec_CAT(A, B) stc_vec_CAT_I(A, B)\n"
          "#define stc_vec(T) stc_vec_CAT(stc_vec_, T)\n"
          "typedef struct { i_key value; } i_type;\n";
  }
  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "tmpl/stc_vec_explicit.h"), EC);
    OS << "// -- template stc_vec_explicit(Name, Elem, Traits)\n"
          "#define stc_vec_explicit(Name, Elem, Traits) Name\n"
          "typedef struct { i_key value; } i_type;\n";
  }
  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "tmpl/stc_hashmap.h"), EC);
    OS << "// -- template stc_hashmap(Key, Val)\n"
          "#define stc_hashmap(Key, Val) stc_hashmap_##Key##_##Val\n"
          "typedef struct { i_key key; i_val value; } i_type;\n";
  }
  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "tmpl/vec.h"), EC);
    OS << "// -- template vec(T)\n"
          "#define vec(T) vec_##T\n"
          "typedef struct { i_key value; } i_type;\n";
  }
  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "other/foo.h"), EC);
    OS << "#ifndef FOO_H\n#define FOO_H\nstruct Foo { int x; };\n#endif\n";
  }
  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "other/pragma.h"), EC);
    OS << "#pragma once\nstruct Pragma { int x; };\n";
  }
  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "other/conditional.h"), EC);
    OS << "#ifndef __cplusplus\nextern \"C\" {\n#endif\n"
          "struct Conditional { int x; };\n";
  }
  {
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "other/defined.h"), EC);
    OS << "#if !defined(BAR_H)\n#define BAR_H\nstruct Bar { int x; };\n#endif\n";
  }

  c_autotmpl::TemplateRegistry Registry;
  Registry.scan(Root);
  assert(!Registry.empty());
  Registry.applyPreset(c_autotmpl::TemplatePreset::StcStatic);

  const c_autotmpl::TemplateDef *Def = Registry.findByName("List");
  assert(Def && Def->Name == "List");
  assert(Def->IncludeFromGenDir == "../tmpl/list.h");

  const c_autotmpl::TemplateDef *MapDef = Registry.findByName("Map");
  assert(MapDef && MapDef->Params.size() == 1 && MapDef->Params[0] == "Elem");

  const c_autotmpl::TemplateDef *SplitDef = Registry.findByName("Split");
  assert(SplitDef && SplitDef->HasTmplHeaderGuard);
  assert(SplitDef->GeneratedImplSource.find("/gen/impl/split.c") !=
         std::string::npos);
  assert(!Def->HasTmplHeaderGuard);

  const c_autotmpl::TemplateDef *StcDef = Registry.findByName("stc_vec");
  assert(StcDef && StcDef->Preset == c_autotmpl::TemplatePreset::StcStatic);
  assert(StcDef->ArgSpecs.size() == 1);
  assert(StcDef->ArgSpecs[0].Role == c_autotmpl::ArgRole::Key);
  assert(c_autotmpl::isTypeArgument(*StcDef, 0));
  assert(c_autotmpl::argumentMacroName(*StcDef, 0) == "i_key");
  assert(c_autotmpl::usesDerivedStcNames(*StcDef));
  assert(c_autotmpl::stcContainerName(*StcDef, {"cstr"}) == "stc_vec_cstr");

  const c_autotmpl::TemplateDef *StcExplicitDef =
      Registry.findByName("stc_vec_explicit");
  assert(StcExplicitDef &&
         StcExplicitDef->Preset == c_autotmpl::TemplatePreset::StcStatic);
  assert(StcExplicitDef->ArgSpecs.size() == 3);
  assert(StcExplicitDef->ArgSpecs[0].Role == c_autotmpl::ArgRole::Name);
  assert(StcExplicitDef->ArgSpecs[1].Role == c_autotmpl::ArgRole::Key);
  assert(StcExplicitDef->ArgSpecs[2].Role == c_autotmpl::ArgRole::Flags);
  assert(!c_autotmpl::isTypeArgument(*StcExplicitDef, 0));
  assert(c_autotmpl::isTypeArgument(*StcExplicitDef, 1));
  assert(!c_autotmpl::isTypeArgument(*StcExplicitDef, 2));
  assert(c_autotmpl::argumentMacroName(*StcExplicitDef, 0) == "i_type");
  assert(c_autotmpl::argumentMacroName(*StcExplicitDef, 1) == "i_key");
  assert(c_autotmpl::argumentMacroName(*StcExplicitDef, 2) == "i_traits");
  assert(!c_autotmpl::usesDerivedStcNames(*StcExplicitDef));

  const c_autotmpl::TemplateDef *StcMapDef =
      Registry.findByName("stc_hashmap");
  assert(StcMapDef && StcMapDef->ArgSpecs.size() == 2);
  assert(StcMapDef->ArgSpecs[0].Role == c_autotmpl::ArgRole::Key);
  assert(StcMapDef->ArgSpecs[1].Role == c_autotmpl::ArgRole::Value);
  assert(c_autotmpl::isTypeArgument(*StcMapDef, 0));
  assert(c_autotmpl::isTypeArgument(*StcMapDef, 1));
  assert(c_autotmpl::argumentMacroName(*StcMapDef, 0) == "i_key");
  assert(c_autotmpl::argumentMacroName(*StcMapDef, 1) == "i_val");
  assert(c_autotmpl::usesDerivedStcNames(*StcMapDef));
  assert(c_autotmpl::stcContainerName(*StcMapDef, {"Foo", "cstr"}) ==
         "stc_hashmap_Foo_cstr");
  assert(c_autotmpl::stcTraits(*StcMapDef, {"Foo", "cstr"}) == "(c_pro_val)");

  c_autotmpl::TemplateRegistry NamedRegistry;
  NamedRegistry.scan(Root);
  SmallVector<StringRef, 1> NamedStcTemplates{"vec"};
  NamedRegistry.applyPreset(c_autotmpl::TemplatePreset::StcStatic,
                            NamedStcTemplates);
  const c_autotmpl::TemplateDef *VecDef = NamedRegistry.findByName("vec");
  assert(VecDef && c_autotmpl::isStcPreset(*VecDef));
  assert(VecDef->ArgSpecs.size() == 1);
  assert(VecDef->ArgSpecs[0].Role == c_autotmpl::ArgRole::Key);
  assert(c_autotmpl::usesDerivedStcNames(*VecDef));
  assert(c_autotmpl::stcContainerName(*VecDef, {"cstr"}) == "vec_cstr");
  assert(NamedRegistry.findByName("stc_vec")->Preset ==
         c_autotmpl::TemplatePreset::None);

  c_autotmpl::TemplateRegistry AutoRegistry;
  AutoRegistry.scan(Root);
  AutoRegistry.applyPreset(c_autotmpl::TemplatePreset::StcStatic, {},
                           /*Auto=*/true);
  assert(c_autotmpl::isStcPreset(*AutoRegistry.findByName("vec")));
  assert(c_autotmpl::isStcPreset(*AutoRegistry.findByName("stc_vec")));

  const c_autotmpl::TemplateDef *SelectDef =
      Registry.findByName("select");
  assert(SelectDef && SelectDef->Kind == c_autotmpl::TemplateKind::Function);
  assert(SelectDef->Params.size() == 2 && SelectDef->Params[0] == "First" &&
         SelectDef->Params[1] == "Second");

  const c_autotmpl::TemplateDef *SumDef = Registry.findByName("sum");
  assert(SumDef && SumDef->Kind == c_autotmpl::TemplateKind::Function);

  const c_autotmpl::TemplateDef *OptionDef = Registry.findByName("Option");
  const c_autotmpl::TemplateDef *UserDef = Registry.findByName("User");
  assert(OptionDef && OptionDef->Kind == c_autotmpl::TemplateKind::Type);
  assert(UserDef && UserDef->Params.size() == 2 &&
         UserDef->Params[0] == "F" && UserDef->Params[1] == "Payload");
  assert(c_autotmpl::isTemplateConstructorArgument("Option", Registry));
  assert(!c_autotmpl::isTemplateConstructorArgument("int", Registry));

  StringRef Type;
  assert(Registry.matchMangled("List_int", Type) == Def && Type == "int");
  assert(Registry.matchMangled("Nope_int", Type) == nullptr);
  // Function templates use explicit call-site spellings only; helper names
  // such as select_impl must not be read as mangled instantiations.
  assert(Registry.matchMangled("select_int__float", Type) == nullptr);

  c_autotmpl::DispatchConfig DispatchCfg;
  DispatchCfg.Enabled = true;
  Registry.applyDispatch(DispatchCfg);
  const c_autotmpl::TemplateDef *DispatchDef =
      Registry.findByName("Dispatch");
  assert(DispatchDef && DispatchDef->DispatchEnabled);
  assert(DispatchDef->DispatchOps.contains("Dispatch_int_push"));
  assert(DispatchDef->DispatchOps.contains("Dispatch_int_set"));
  assert(DispatchDef->DispatchOps.contains("Dispatch_clear"));
  assert(DispatchDef->DispatchOps.contains("observe"));
  assert(!DispatchDef->DispatchOps.contains("helper"));
  const c_autotmpl::TemplateDef *CrateDef = Registry.findByName("Crate");
  assert(CrateDef && CrateDef->DispatchEnabled);
  assert(CrateDef->DispatchOps.contains("crate_alloc"));
  assert(CrateDef->DispatchOps.contains("crate_free"));
  assert(CrateDef->DispatchOps.contains("observe"));
  assert(!CrateDef->DispatchOps.contains("secret"));
  assert(!CrateDef->DispatchOps.contains("_Alignas"));

  // `observe` is an ordinary op shared by two owners. Plural lookup and bare
  // dispatch scanning must see both, exactly once per template.
  SmallVector<const c_autotmpl::TemplateDef *, 2> ObserveOwners;
  Registry.findDispatchOps("observe", ObserveOwners);
  bool SawCrateOwner = false;
  bool SawDispatchOwner = false;
  for (const c_autotmpl::TemplateDef *Owner : ObserveOwners) {
    SawCrateOwner |= Owner == CrateDef;
    SawDispatchOwner |= Owner == DispatchDef;
  }
  assert(ObserveOwners.size() == 2 && SawCrateOwner && SawDispatchOwner);
  {
    c_autotmpl::setSourceScanDispatchTruncate(true);
    SmallVector<c_autotmpl::FoundUsage, 4> SharedOrdinary;
    // The comment keeps the cheap template-name prefilter happy without
    // creating a nested template application to expand.
    c_autotmpl::scanUsages("observe(int) /* Dispatch */;", Registry,
                           SharedOrdinary);
    c_autotmpl::setSourceScanDispatchTruncate(false);
    unsigned CrateUses = 0;
    unsigned DispatchUses = 0;
    for (const c_autotmpl::FoundUsage &Usage : SharedOrdinary) {
      // Header dependency expansion also sees the fixture declarations; only
      // the direct call usages matter here.
      if (Usage.IsNested)
        continue;
      if (Usage.Template == CrateDef)
        ++CrateUses;
      if (Usage.Template == DispatchDef)
        ++DispatchUses;
      assert(Usage.TypeSpelling == "int");
    }
    assert(CrateUses == 1 && DispatchUses == 1);
  }
  auto AllocTarget = CrateDef->DispatchTargets.find("crate_alloc");
  assert(AllocTarget != CrateDef->DispatchTargets.end());
  assert(AllocTarget->second.Base == "crate_alloc" &&
         AllocTarget->second.AppendArgs);
  auto ObserveTarget = CrateDef->DispatchTargets.find("observe");
  assert(ObserveTarget != CrateDef->DispatchTargets.end());
  assert(ObserveTarget->second.Base == "observe");
  auto AllocOffset = CrateDef->DispatchOpOffsets.find("crate_alloc");
  assert(AllocOffset != CrateDef->DispatchOpOffsets.end());
  auto CrateHeader = MemoryBuffer::getFile(CrateDef->Header);
  assert(CrateHeader);
  assert(StringRef((*CrateHeader)->getBuffer())
             .substr(AllocOffset->second)
             .starts_with("crate_alloc"));
  SmallVector<c_autotmpl::DispatchCase, 1> CrateCases{
      {"int", "Crate_int", {"int"}}};
  SmallVector<c_autotmpl::DispatchCase, 1> DispatchCases{
      {"int", "Dispatch_int"}};
  SmallVector<StringRef, 3> CrateOps{"crate_alloc", "crate_free", "observe"};
  SmallVector<StringRef, 1> DispatchOps{"Dispatch_int_push"};

  auto copyCases = [](ArrayRef<c_autotmpl::DispatchCase> Cases) {
    return std::vector<c_autotmpl::DispatchCase>(Cases.begin(), Cases.end());
  };
  auto makeGroup = [&](StringRef Key, const c_autotmpl::TemplateDef *Owner,
                       ArrayRef<c_autotmpl::DispatchCase> Cases) {
    c_autotmpl::DispatchSharedGroup Group;
    Group.Key = Key.str();
    Group.SelectorOp = Key.str();
    Group.Contribs.push_back({Owner, nullptr, copyCases(Cases), Key.str()});
    return Group;
  };

  // Two owners of `crate_alloc`: the shared PICK enters Crate's table, whose
  // base selector falls through to Dispatch's single-case table.
  c_autotmpl::DispatchSharedGroup CrateAllocGroup =
      makeGroup("crate_alloc", CrateDef, CrateCases);
  CrateAllocGroup.Contribs.push_back(
      {DispatchDef, nullptr, copyCases(DispatchCases), "crate_alloc"});
  std::string CrateSection = c_autotmpl::makeDispatchSection(
      *CrateDef, CrateCases, CrateOps, DispatchCfg, {CrateAllocGroup});
  for (StringRef Needle : {
           "/* c-autotmpl dispatch begin Crate */",
           "/* c-autotmpl generated-by=plugin */",
           "Crate_int: crate_alloc_int", "Crate_int: crate_free_int",
           "Crate_int: observe",
           "#define C_AUTOTMPL_DISPATCH_SEL_Crate_crate_alloc_1(p1) "
           "C_AUTOTMPL_DISPATCH_SEL_Dispatch_crate_alloc_1_1(p1)",
           "#define C_AUTOTMPL_DISPATCH_PICK_Crate_crate_alloc(",
           "/* c-autotmpl dispatch end Crate */"})
    assert(CrateSection.find(Needle.str()) != std::string::npos);

  c_autotmpl::DispatchSharedGroup ObserveGroup =
      makeGroup("observe", CrateDef, CrateCases);
  ObserveGroup.Contribs.push_back(
      {DispatchDef, nullptr, copyCases(DispatchCases), "observe"});
  std::string ObserveOwnerSection = c_autotmpl::makeDispatchSection(
      *CrateDef, CrateCases, CrateOps, DispatchCfg, {ObserveGroup});
  assert(ObserveOwnerSection.find(
             "#define C_AUTOTMPL_DISPATCH_SEL_Crate_observe_1(p1) "
             "C_AUTOTMPL_DISPATCH_SEL_Dispatch_observe_1_1(p1)") !=
         std::string::npos);
  // Owner sections carry selectors only; every public spelling lives in one
  // shared section exactly once.
  for (StringRef Public : {"#define $crate_alloc(", "#define $crate_free(",
                           "#define $observe("})
    assert(countOccurrences(CrateSection, Public) == 0);

  std::string SharedAlloc =
      c_autotmpl::makeSharedOpSection(CrateAllocGroup, DispatchCfg);
  for (StringRef Needle : {
           "/* c-autotmpl dispatch begin shared.crate_alloc */",
           "/* c-autotmpl generated-by=plugin */",
           "#define $crate_alloc(...) "
           "C_AUTOTMPL_DISPATCH_PICK_Crate_crate_alloc(__VA_ARGS__,",
           "C_AUTOTMPL_DISPATCH_SEL_Crate_crate_alloc_1_1)(__VA_ARGS__)"
           "(__VA_ARGS__)",
           "/* c-autotmpl dispatch end shared.crate_alloc */"})
    assert(SharedAlloc.find(Needle.str()) != std::string::npos);
  assert(countOccurrences(SharedAlloc, "#define $crate_alloc(") == 1);
  assert(countOccurrences(CrateSection + SharedAlloc,
                          "#define $crate_alloc(") == 1);

  auto PushOffset = DispatchDef->DispatchOpOffsets.find("Dispatch_int_push");
  assert(PushOffset != DispatchDef->DispatchOpOffsets.end());
  auto DispatchHeader = MemoryBuffer::getFile(DispatchDef->Header);
  assert(DispatchHeader);
  assert(StringRef((*DispatchHeader)->getBuffer())
             .substr(PushOffset->second)
             .starts_with("Dispatch_int_push"));
  c_autotmpl::DispatchSharedGroup PushGroup =
      makeGroup("Dispatch_int_push", DispatchDef, DispatchCases);
  std::string DispatchSection = c_autotmpl::makeDispatchSection(
      *DispatchDef, DispatchCases, DispatchOps, DispatchCfg);
  for (StringRef Needle : {
           "/* c-autotmpl dispatch begin Dispatch */",
           "/* c-autotmpl generated-by=plugin */",
           "#ifdef C_AUTOTMPL_Dispatch_int",
           "_Generic((p1), Dispatch_int: Dispatch_int_push",
           "Dispatch_int*: Dispatch_int_push",
           "#define C_AUTOTMPL_DISPATCH_PICK_Dispatch_Dispatch_int_push(",
           "/* c-autotmpl dispatch end Dispatch */"})
    assert(DispatchSection.find(Needle.str()) != std::string::npos);
  assert(countOccurrences(DispatchSection, "#define $Dispatch_int_push(") == 0);

  DispatchCfg.Style = c_autotmpl::DispatchStyle::Postfix;
  DispatchCfg.Affix = "_method";
  std::string PostfixSection = c_autotmpl::makeSharedOpSection(PushGroup,
                                                               DispatchCfg);
  assert(PostfixSection.find("#define Dispatch_int_push_method(...)") !=
         std::string::npos);
  assert(PostfixSection.find("#define $Dispatch_int_push(...)") ==
         std::string::npos);
  DispatchCfg.Style = c_autotmpl::DispatchStyle::Prefix;
  DispatchCfg.Affix = "$";

  // Central-header layout and ownership: CLI sections survive plugin writes,
  // pruning and stripping; plugin sections carry the plugin marker.
  assert(c_autotmpl::generatedDispatchPath(join(Root, "gen/vec.h")) ==
         join(Root, "gen/dispatch.h"));
  sys::fs::create_directories(join(Root, "gen"));
  std::string CentralPath = join(Root, "gen/central-dispatch.h");
  {
    std::error_code EC;
    raw_fd_ostream OS(CentralPath, EC);
    OS << "#ifndef CENTRAL_DISPATCH_H\n#define CENTRAL_DISPATCH_H\n\n"
          "/* c-autotmpl dispatch begin Crate */\n"
          "/* c-autotmpl generated-by=cli */\n"
          "#define CLI_OWNED 1\n"
          "/* c-autotmpl dispatch end Crate */\n\n"
          "#endif\n";
  }
  c_autotmpl::Instantiator DispatchInst;
  std::string DispatchError;
  assert(!DispatchInst.writeDispatchSection(CentralPath, "Crate", CrateSection,
                                            DispatchError));
  assert(DispatchError.empty());
  std::string CentralText = readText(CentralPath);
  assert(CentralText.find("/* c-autotmpl generated-by=cli */") !=
         std::string::npos);
  assert(CentralText.find("#define CLI_OWNED 1") != std::string::npos);
  assert(countOccurrences(CentralText, "generated-by=plugin") == 0);

  assert(DispatchInst.writeDispatchSection(CentralPath, "shared.crate_alloc",
                                           SharedAlloc, DispatchError));
  assert(DispatchError.empty());
  assert(!DispatchInst.writeDispatchSection(CentralPath, "shared.crate_alloc",
                                            SharedAlloc, DispatchError));
  std::string ObserveSection = c_autotmpl::makeSharedOpSection(
      makeGroup("observe", CrateDef, CrateCases), DispatchCfg);
  assert(DispatchInst.writeDispatchSection(CentralPath, "shared.observe",
                                           ObserveSection, DispatchError));
  llvm::StringSet<> Keep;
  Keep.insert("Crate");
  Keep.insert("shared.crate_alloc");
  std::vector<std::string> Removed;
  assert(DispatchInst.pruneDispatchSections(CentralPath, Keep, Removed,
                                            DispatchError));
  assert(Removed.size() == 1 && Removed.front() == "shared.observe");
  CentralText = readText(CentralPath);
  assert(CentralText.find("/* c-autotmpl dispatch begin shared.observe */") ==
         std::string::npos);
  assert(CentralText.find("#define CLI_OWNED 1") != std::string::npos);
  assert(countOccurrences(CentralText, "#define $crate_alloc(") == 1);

  std::string LegacyPath = join(Root, "gen/legacy-dispatch.h");
  {
    std::error_code EC;
    raw_fd_ostream OS(LegacyPath, EC);
    OS << "/* c-autotmpl dispatch begin Legacy */\n"
          "/* c-autotmpl generated-by=plugin */\n"
          "#define LEGACY_PLUGIN 1\n"
          "/* c-autotmpl dispatch end Legacy */\n"
          "/* c-autotmpl dispatch begin Kept */\n"
          "/* c-autotmpl generated-by=cli */\n"
          "#define LEGACY_CLI 1\n"
          "/* c-autotmpl dispatch end Kept */\n";
  }
  assert(DispatchInst.stripDispatchSections(LegacyPath, DispatchError));
  assert(DispatchError.empty());
  std::string LegacyText = readText(LegacyPath);
  assert(LegacyText.find("LEGACY_PLUGIN") == std::string::npos);
  assert(LegacyText.find("LEGACY_CLI") != std::string::npos);
  assert(!DispatchInst.stripDispatchSections(LegacyPath, DispatchError));

  DispatchCfg.Templates.push_back("Nope");
  Registry.applyDispatch(DispatchCfg);
  assert(!DispatchDef->DispatchEnabled);
  DispatchCfg.Templates[0] = "Dispatch";
  Registry.applyDispatch(DispatchCfg);
  assert(DispatchDef->DispatchEnabled);

  std::string DispatchPath = join(Root, "gen/dispatch.h");
  assert(DispatchInst.ensureDispatch(*DispatchDef, DispatchCases, DispatchOps,
                                     DispatchCfg, DispatchPath,
                                     DispatchError));
  assert(DispatchError.empty());
  assert(!DispatchInst.ensureDispatch(*DispatchDef, DispatchCases, DispatchOps,
                                      DispatchCfg, DispatchPath,
                                      DispatchError));
  std::string SharedPush =
      c_autotmpl::makeSharedOpSection(PushGroup, DispatchCfg);
  assert(DispatchInst.writeDispatchSection(
      DispatchPath, "shared.Dispatch_int_push", SharedPush, DispatchError));
  auto DispatchBuffer = MemoryBuffer::getFile(DispatchPath);
  assert(DispatchBuffer);
  StringRef DispatchText = (*DispatchBuffer)->getBuffer();
  assert(DispatchText.contains("/* c-autotmpl dispatch begin Dispatch */"));
  assert(DispatchText.contains(
      "/* c-autotmpl dispatch begin shared.Dispatch_int_push */"));
  assert(countOccurrences(DispatchText, "#define $Dispatch_int_push(") == 1);

  c_autotmpl::Instantiator Instantiator;
  std::string GenPath, Error;
  assert(Instantiator.instantiate(*Def, "int", "", GenPath, Error) ==
         c_autotmpl::InstantiationResult::Added);
  assert(Instantiator.instantiate(*Def, "int", "", GenPath, Error) ==
         c_autotmpl::InstantiationResult::Existing);
  const std::string FooGuard = c_autotmpl::findIncludeGuard(join(Root, "other/foo.h"));
  assert(FooGuard == "FOO_H");
  assert(c_autotmpl::findIncludeGuard(join(Root, "other/pragma.h")).empty());
  assert(c_autotmpl::findIncludeGuard(join(Root, "other/conditional.h")).empty());
  assert(c_autotmpl::findIncludeGuard(join(Root, "other/defined.h")) == "BAR_H");
  assert(Instantiator.instantiate(*Def, "struct Foo", FooGuard, GenPath,
                                  Error) ==
         c_autotmpl::InstantiationResult::Added);

  auto BufferOrErr = MemoryBuffer::getFile(GenPath);
  assert(BufferOrErr);
  StringRef Content = (*BufferOrErr)->getBuffer();
  assert(Content.find("#define T int") != StringRef::npos);
  assert(Content.find("#ifndef C_AUTOTMPL_List_struct_20Foo") !=
         StringRef::npos);
  assert(Content.find("#ifdef FOO_H") != StringRef::npos);
  assert(Content.find("#define T struct Foo") != StringRef::npos);

  std::string SplitPath, SplitError;
  assert(Instantiator.instantiate(*SplitDef, "int", "", SplitPath,
                                  SplitError) ==
         c_autotmpl::InstantiationResult::Added);
  {
    auto SplitBuffer = MemoryBuffer::getFile(SplitPath);
    assert(SplitBuffer);
    StringRef SplitContent = (*SplitBuffer)->getBuffer();
    assert(SplitContent.find("/* c-autotmpl: Split(int) */") !=
           StringRef::npos);
    assert(SplitContent.find("#define T int\n#define TMPL_HEADER 1\n") !=
           StringRef::npos);
    assert(SplitContent.find("#undef TMPL_HEADER\n#undef T\n") !=
           StringRef::npos);
  }

  {
    std::vector<std::string> Args{"Foo"};
    std::vector<std::string> Guards{FooGuard};
    std::vector<std::string> Includes{"#include \"../../other/foo.h\""};
    std::string ImplPath;
    std::string ImplError;
    assert(Instantiator.instantiateImpl(*SplitDef, "Foo", Args, Guards,
                                        Includes, ImplPath, ImplError) ==
           c_autotmpl::InstantiationResult::Added);
    assert(ImplPath.find("/gen/impl/split.c") != std::string::npos);
    assert(Instantiator.instantiateImpl(*SplitDef, "Foo", Args, Guards,
                                        Includes, ImplPath, ImplError) ==
           c_autotmpl::InstantiationResult::Existing);
    auto ImplBuffer = MemoryBuffer::getFile(ImplPath);
    assert(ImplBuffer);
    StringRef ImplContent = (*ImplBuffer)->getBuffer();
    assert(ImplContent.find("/* Generated by c-autotmpl. Do not edit. */\n"
                            "#undef TMPL_HEADER\n"
                            "#include \"../../other/foo.h\"\n") !=
           StringRef::npos);
    assert(ImplContent.find("/* c-autotmpl: Split(Foo) */") !=
           StringRef::npos);
    assert(ImplContent.find("#ifndef C_AUTOTMPL_IMPL_Split_Foo") !=
           StringRef::npos);
    assert(ImplContent.find("#ifdef FOO_H\n") != StringRef::npos);
    assert(ImplContent.find("#define T Foo\n") != StringRef::npos);
    assert(ImplContent.find("#include \"../../tmpl/split.h\"\n#undef T\n") !=
           StringRef::npos);
    assert(ImplContent.find("#define TMPL_HEADER") == StringRef::npos);
  }

  std::string SelectPath, SelectError;
  assert(Instantiator.instantiate(*SelectDef, "int, Foo", FooGuard,
                                  SelectPath, SelectError) ==
         c_autotmpl::InstantiationResult::Added);
  assert(Instantiator.instantiate(*SelectDef, "int, Foo", FooGuard,
                                  SelectPath, SelectError) ==
         c_autotmpl::InstantiationResult::Existing);
  {
    auto SelectBuffer = MemoryBuffer::getFile(SelectPath);
    assert(SelectBuffer);
    StringRef SelectContent = (*SelectBuffer)->getBuffer();
    assert(SelectContent.find("/* c-autotmpl: select(int, Foo) */") !=
           StringRef::npos);
    assert(SelectContent.find("#ifndef C_AUTOTMPL_select_int_2C_20Foo") !=
           StringRef::npos);
    assert(SelectContent.find("#ifdef FOO_H") != StringRef::npos);
    assert(SelectContent.find("#define First int") != StringRef::npos);
    assert(SelectContent.find("#define Second Foo") != StringRef::npos);
    assert(SelectContent.find("#undef Second\n#undef First") !=
           StringRef::npos);
  }

  {
    // A stale hand-written block beside the generated one must be removed,
    // even though the generated block is still wanted.
    std::error_code EC;
    raw_fd_ostream OS(GenPath, EC, sys::fs::CD_CreateAlways);
    OS << "#ifndef GEN_LIST_H\n#define GEN_LIST_H\n\n"
          "/* c-autotmpl: List(int) */\n"
          "#define T int\n#include \"../tmpl/list.h\"\n#undef T\n\n"
          "#define T int\n#include \"../tmpl/list.h\"\n#undef T\n\n"
          "#endif\n";
  }
  {
    // Use a fresh cache after the external rewrite to test pruning itself;
    // mtime-based invalidation has its own integration coverage.
    c_autotmpl::Instantiator PruneInst;
    StringSet<> Keep;
    Keep.insert(c_autotmpl::instantiationMarker("List", "int"));
    std::vector<std::string> Removed;
    std::string PruneError;
    assert(PruneInst.prune(*Def, Keep, Removed, PruneError));
    assert(Removed.empty());
  }
  {
    auto Buffer = MemoryBuffer::getFile(GenPath);
    assert(Buffer);
    StringRef Pruned = (*Buffer)->getBuffer();
    assert(Pruned.find("/* c-autotmpl: List(int) */") != StringRef::npos);
    assert(Pruned.count("#define T int") == 1);
  }

  std::string MapPath, MapError;
  {
    // A hand-written instantiation must be recognized as already present.
    std::error_code EC;
    raw_fd_ostream OS(join(Root, "gen/map.h"), EC);
    OS << "#ifndef GEN_MAP_H\n#define GEN_MAP_H\n\n"
          "#define Elem char\n#include \"../tmpl/map.h\"\n#undef Elem\n\n"
          "#endif\n";
  }
  assert(Instantiator.instantiate(*MapDef, "char", "", MapPath, MapError) ==
         c_autotmpl::InstantiationResult::Existing);
  {
    auto Buffer = MemoryBuffer::getFile(MapPath);
    assert(Buffer);
    StringRef Content = (*Buffer)->getBuffer();
    assert(Content.find("/* c-autotmpl:") == StringRef::npos);
    assert(Content.find("#define Elem char") != StringRef::npos);
  }

  {
    // External edits to a generated header must invalidate the cache.
    std::error_code EC;
    raw_fd_ostream OS(MapPath, EC, sys::fs::CD_CreateAlways);
    OS << "#ifndef GEN_MAP_H\n#define GEN_MAP_H\n\n#endif\n";
  }
  assert(Instantiator.instantiate(*MapDef, "char", "", MapPath, MapError) ==
         c_autotmpl::InstantiationResult::Added);
  auto MapBuffer = MemoryBuffer::getFile(MapPath);
  assert(MapBuffer);
  StringRef MapContent = (*MapBuffer)->getBuffer();
  assert(MapContent.find("/* c-autotmpl: Map(char) */") != StringRef::npos);
  assert(MapContent.find("#define Elem char") != StringRef::npos);
  assert(MapContent.find("#undef Elem") != StringRef::npos);

  {
    SmallVector<c_autotmpl::FoundUsage, 8> Functions;
    c_autotmpl::scanUsages(
        "select(int, Foo) a; select(int, float) b; select_impl(x);",
        Registry, Functions);
    assert(Functions.size() == 2);
    assert(Functions[0].Template == SelectDef);
    assert(Functions[0].TypeSpelling == "int, Foo");
    assert(Functions[0].Args.size() == 2 && Functions[0].Args[0] == "int" &&
           Functions[0].Args[1] == "Foo");
    assert(Functions[1].TypeSpelling == "int, float");
    assert(Functions[1].Args.size() == 2);

    SmallVector<c_autotmpl::FoundUsage, 8> Placeholders;
    c_autotmpl::scanUsages(
        "static inline int select(First, Second)(First a, Second b);",
        Registry, Placeholders);
    assert(Placeholders.empty());

    SmallVector<c_autotmpl::FoundUsage, 8> WrongArity;
    c_autotmpl::scanUsages("select(int) a; select(int, float, char) b;",
                           Registry, WrongArity);
    assert(WrongArity.empty());

    SmallVector<c_autotmpl::FoundUsage, 8> TypePlaceholders;
    c_autotmpl::scanUsages("List(T) p; User(F, Payload) u; List(int) q;",
                           Registry, TypePlaceholders);
    assert(TypePlaceholders.size() == 1);
    assert(TypePlaceholders[0].Template == Def);
    assert(TypePlaceholders[0].TypeSpelling == "int");

    SmallVector<c_autotmpl::FoundUsage, 8> NestedFunction;
    c_autotmpl::scanUsages("select(Box(int), float) n;", Registry,
                           NestedFunction);
    assert(NestedFunction.size() == 2);
    assert(NestedFunction[0].Template == Registry.findByName("Box"));
    assert(NestedFunction[0].IsNested);
    assert(NestedFunction[1].Template == SelectDef);
    assert(NestedFunction[1].TypeSpelling == "Box_int, float");
    assert(NestedFunction[1].Args.size() == 2);
    assert(NestedFunction[1].Args[0] == "Box_int");
    assert(NestedFunction[1].DependsOn.size() == 1);

    // A function header that spells out a concrete type in its function head
    // creates a dependency without any type deduction.
    SmallVector<c_autotmpl::FoundUsage, 8> HeaderDeps;
    c_autotmpl::scanUsages("sum(int) s;", Registry, HeaderDeps);
    assert(HeaderDeps.size() == 2);
    assert(HeaderDeps[0].Template == Def);
    assert(HeaderDeps[0].TypeSpelling == "int");
    assert(HeaderDeps[0].IsNested);
    assert(HeaderDeps[1].Template == SumDef);
    assert(HeaderDeps[1].TypeSpelling == "int");
    assert(HeaderDeps[1].DependsOn.size() == 1);
    assert(HeaderDeps[1].DependsOn[0].Template == Def);
    assert(HeaderDeps[1].DependsOn[0].TypeSpelling == "int");

    // A bare registered template name is a lexical constructor. Substituting
    // it into the type header reveals concrete applications such as
    // Option(int), which become dependencies.
    SmallVector<c_autotmpl::FoundUsage, 8> Hkt;
    c_autotmpl::scanUsages("User(Option, int) user;", Registry, Hkt);
    assert(Hkt.size() == 2);
    assert(Hkt[0].Template == OptionDef);
    assert(Hkt[0].TypeSpelling == "int");
    assert(Hkt[0].IsNested);
    assert(Hkt[1].Template == UserDef);
    assert(Hkt[1].TypeSpelling == "Option, int");
    assert(Hkt[1].Args.size() == 2 && Hkt[1].Args[0] == "Option" &&
           Hkt[1].Args[1] == "int");
    assert(Hkt[1].DependsOn.size() == 1);
    assert(Hkt[1].DependsOn[0].Template == OptionDef);
    assert(Hkt[1].DependsOn[0].TypeSpelling == "int");

    SmallVector<c_autotmpl::FoundUsage, 8> SingleHkt;
    c_autotmpl::scanUsages("Wrap(Option) wrap;", Registry, SingleHkt);
    assert(SingleHkt.size() == 2);
    assert(SingleHkt[0].Template == OptionDef);
    assert(SingleHkt[0].TypeSpelling == "int");
    assert(SingleHkt[1].Template == Registry.findByName("Wrap"));
    assert(SingleHkt[1].TypeSpelling == "Option");
    assert(SingleHkt[1].DependsOn.size() == 1);

    // Derived STC wrappers read like ordinary parametric types. Names and
    // trait flags are no longer present at the call site.
    SmallVector<c_autotmpl::FoundUsage, 8> StcUsages;
    c_autotmpl::scanUsages("stc_vec(Foo) v; stc_vec(cstr) s;", Registry,
                           StcUsages);
    assert(StcUsages.size() == 2);
    assert(StcUsages[0].Template == StcDef);
    assert(StcUsages[0].TypeSpelling == "Foo");
    assert(StcUsages[0].Args.size() == 1);
    assert(StcUsages[0].Args[0] == "Foo");
    assert(StcUsages[1].TypeSpelling == "cstr");
    assert(StcUsages[1].Args[0] == "cstr");

    SmallVector<c_autotmpl::FoundUsage, 8> StcAdapterUsages;
    c_autotmpl::scanUsages("stc_vec_push(Foo)(&v, 7);", Registry,
                           StcAdapterUsages);
    assert(StcAdapterUsages.size() == 1);
    assert(StcAdapterUsages[0].Template == StcDef);
    assert(StcAdapterUsages[0].TypeSpelling == "Foo");
    assert(StcAdapterUsages[0].AdapterOp == "push");

    SmallVector<c_autotmpl::FoundUsage, 8> StcExplicitUsages;
    c_autotmpl::scanUsages(
        "stc_vec_explicit(LegacyVec, Foo, (c_pro_key | c_use_eq)) v;",
        Registry, StcExplicitUsages);
    assert(StcExplicitUsages.size() == 1);
    assert(StcExplicitUsages[0].Template == StcExplicitDef);
    assert(StcExplicitUsages[0].TypeSpelling ==
           "LegacyVec, Foo, (c_pro_key | c_use_eq)");
    assert(StcExplicitUsages[0].Args[0] == "LegacyVec");
    assert(StcExplicitUsages[0].Args[2] == "(c_pro_key | c_use_eq)");

    SmallVector<c_autotmpl::FoundUsage, 8> StcMapUsages;
    c_autotmpl::scanUsages("stc_hashmap(Foo, cstr) m;", Registry,
                           StcMapUsages);
    assert(StcMapUsages.size() == 1);
    assert(StcMapUsages[0].Template == StcMapDef);
    assert(StcMapUsages[0].Args.size() == 2);
    assert(StcMapUsages[0].Args[0] == "Foo");
    assert(StcMapUsages[0].Args[1] == "cstr");
  }

  {
    std::string UserPath, UserError;
    assert(Instantiator.instantiate(*UserDef, "Option, int", "", UserPath,
                                    UserError) ==
           c_autotmpl::InstantiationResult::Added);
    auto UserBuffer = MemoryBuffer::getFile(UserPath);
    assert(UserBuffer);
    StringRef UserContent = (*UserBuffer)->getBuffer();
    assert(UserContent.find("/* c-autotmpl: User(Option, int) */") !=
           StringRef::npos);
    assert(UserContent.find("#define F Option") != StringRef::npos);
    assert(UserContent.find("#define Payload int") != StringRef::npos);
    assert(UserContent.find("#undef Payload\n#undef F") != StringRef::npos);
  }

  {
    std::string StcPath, StcError;
    SmallVector<StringRef, 1> StcOps{"push"};
    assert(Instantiator.instantiate(*StcDef, "Foo", FooGuard, StcPath,
                                    StcError, /*DryRun=*/false, {}, {},
                                    StcOps) ==
           c_autotmpl::InstantiationResult::Added);
    auto StcBuffer = MemoryBuffer::getFile(StcPath);
    assert(StcBuffer);
    StringRef StcContent = (*StcBuffer)->getBuffer();
    assert(StcContent.find("/* c-autotmpl: stc_vec(Foo) */") !=
           StringRef::npos);
    assert(StcContent.find("#ifdef FOO_H") != StringRef::npos);
    assert(StcContent.find("#define i_type stc_vec_Foo\n#define i_key Foo\n") !=
           StringRef::npos);
    assert(StcContent.find("#undef i_key\n#undef i_type\n") !=
           StringRef::npos);
    assert(StcContent.find("#define i_traits") == StringRef::npos);
    assert(StcContent.find(
               "#define stc_vec_push(...) "
               "C_AUTOTMPL_STC_ADAPTER_CAT(stc_vec(__VA_ARGS__), _push)\n") !=
           StringRef::npos);
    assert(StcContent.find("#define TMPL_HEADER") == StringRef::npos);
    assert(StcContent.find("#define T Foo") == StringRef::npos);

    std::string StcBuiltinPath;
    assert(Instantiator.instantiate(*StcDef, "cstr", "", StcBuiltinPath,
                                    StcError) ==
           c_autotmpl::InstantiationResult::Added);
    auto StcBuiltinBuffer = MemoryBuffer::getFile(StcBuiltinPath);
    assert(StcBuiltinBuffer);
    StringRef StcBuiltinContent = (*StcBuiltinBuffer)->getBuffer();
    size_t BuiltinMarker =
        StcBuiltinContent.find("/* c-autotmpl: stc_vec(cstr) */");
    assert(BuiltinMarker != StringRef::npos);
    assert(StcBuiltinContent.substr(BuiltinMarker).find("#ifdef") ==
           StringRef::npos);
    assert(StcBuiltinContent.find(
               "#define i_type stc_vec_cstr\n#define i_key cstr\n"
               "#define i_traits (c_pro_key | c_use_eq)\n") !=
           StringRef::npos);
    assert(StcBuiltinContent.find(
               "#undef i_traits\n#undef i_key\n#undef i_type\n") !=
           StringRef::npos);

    std::string StcLegacyPath;
    assert(Instantiator.instantiate(
               *StcExplicitDef, "LegacyVec, Foo, (c_pro_key | c_use_eq)",
               FooGuard, StcLegacyPath, StcError) ==
           c_autotmpl::InstantiationResult::Added);
    auto StcLegacyBuffer = MemoryBuffer::getFile(StcLegacyPath);
    assert(StcLegacyBuffer);
    StringRef StcLegacyContent = (*StcLegacyBuffer)->getBuffer();
    assert(StcLegacyContent.find(
               "#define i_type LegacyVec\n#define i_key Foo\n"
               "#define i_traits (c_pro_key | c_use_eq)\n") !=
           StringRef::npos);
    assert(StcLegacyContent.find(
               "#undef i_traits\n#undef i_key\n#undef i_type\n") !=
           StringRef::npos);

    std::string StcMapPath;
    assert(Instantiator.instantiate(*StcMapDef, "Foo, cstr", FooGuard,
                                    StcMapPath, StcError) ==
           c_autotmpl::InstantiationResult::Added);
    auto StcMapBuffer = MemoryBuffer::getFile(StcMapPath);
    assert(StcMapBuffer);
    StringRef StcMapContent = (*StcMapBuffer)->getBuffer();
    assert(StcMapContent.find(
               "#define i_type stc_hashmap_Foo_cstr\n#define i_key Foo\n"
               "#define i_val cstr\n#define i_traits (c_pro_val)\n") !=
           StringRef::npos);
    assert(StcMapContent.find(
               "#undef i_traits\n#undef i_val\n#undef i_key\n#undef i_type\n") !=
           StringRef::npos);
  }

  {
    // Nested applications expand inner types first. The already-mangled
    // spelling must be recognized too, so no call-site alias is needed.
    const c_autotmpl::TemplateDef *BoxDef = Registry.findByName("Box");
    assert(BoxDef && BoxDef->Name == "Box");

    SmallVector<c_autotmpl::FoundUsage, 8> Usages;
    c_autotmpl::scanUsages("List(Box(int)) nested;", Registry, Usages);
    assert(Usages.size() == 2);
    assert(Usages[0].Template == BoxDef);
    assert(Usages[0].TypeSpelling == "int");
    assert(Usages[1].Template == Def);
    assert(Usages[1].TypeSpelling == "Box_int");
    assert(Usages[1].DependsOn.size() == 1);

    SmallVector<c_autotmpl::FoundUsage, 8> Mangled;
    c_autotmpl::scanUsages("List(Box_int) mangled;", Registry, Mangled);
    assert(Mangled.size() == 2);
    assert(Mangled[0].Template == BoxDef);
    assert(Mangled[1].Template == Def);
    assert(Mangled[1].TypeSpelling == "Box_int");
    assert(Mangled[1].DependsOn.size() == 1);

    std::string BoxPath, NestedError;
    assert(Instantiator.instantiate(*BoxDef, "int", "", BoxPath, NestedError) ==
           c_autotmpl::InstantiationResult::Added);

    SmallVector<c_autotmpl::DependencyGuard, 2> Deps;
    Deps.push_back({c_autotmpl::generatedHeaderFileGuard(BoxPath)});

    std::string NestedPath;
    assert(Instantiator.instantiate(*Def, "Box_int", "", NestedPath,
                                    NestedError, false, Deps) ==
           c_autotmpl::InstantiationResult::Added);
    auto NestedBuffer = MemoryBuffer::getFile(NestedPath);
    assert(NestedBuffer);
    StringRef Nested = (*NestedBuffer)->getBuffer();
    assert(Nested.find("#ifndef C_AUTOTMPL_List_Box_int") !=
           StringRef::npos);
    assert(Nested.find("#ifdef GEN_BOX_H") != StringRef::npos);
    assert(Nested.find("#define T Box_int") != StringRef::npos);
    assert(Instantiator.instantiate(*Def, "Box_int", "", NestedPath,
                                    NestedError, false, Deps) ==
           c_autotmpl::InstantiationResult::Existing);

    // A deeper chain must be flattened into the outermost header. Otherwise
    // Box(List_char) and List(Box_List_char) guard each other's headers and
    // whichever is included first leaves the other undefined.
    SmallVector<c_autotmpl::FoundUsage, 8> Deep;
    c_autotmpl::scanUsages("List(Box(List(char))) deep;", Registry, Deep);
    assert(Deep.size() == 3);
    assert(Deep[0].IsNested && Deep[1].IsNested && !Deep[2].IsNested);
    assert(Deep[0].Template == Def && Deep[0].TypeSpelling == "char");
    assert(Deep[1].Template == BoxDef && Deep[1].TypeSpelling == "List_char");
    assert(Deep[2].Template == Def &&
           Deep[2].TypeSpelling == "Box_List_char");

    std::string DeepPath;
    SmallVector<c_autotmpl::DependencyGuard, 2> OuterDeps;
    OuterDeps.push_back(
        {c_autotmpl::generatedHeaderFileGuard(BoxDef->GeneratedHeader)});
    assert(Instantiator.instantiate(*Deep[0].Template, Deep[0].TypeSpelling,
                                    "", DeepPath, NestedError, false, {},
                                    Def->GeneratedHeader) ==
           c_autotmpl::InstantiationResult::Added);
    assert(Instantiator.instantiate(*Deep[1].Template, Deep[1].TypeSpelling,
                                    "", DeepPath, NestedError, false, {},
                                    Def->GeneratedHeader) ==
           c_autotmpl::InstantiationResult::Added);
    SmallVector<c_autotmpl::DependencyGuard, 2> InnerDeps;
    InnerDeps.push_back(
        {c_autotmpl::generatedHeaderFileGuard(Def->GeneratedHeader)});
    assert(Instantiator.instantiate(*Deep[1].Template, Deep[1].TypeSpelling,
                                    "", DeepPath, NestedError, false, InnerDeps,
                                    Deep[1].Template->GeneratedHeader) ==
           c_autotmpl::InstantiationResult::Added);
    assert(Instantiator.instantiate(*Deep[2].Template, Deep[2].TypeSpelling,
                                    "", DeepPath, NestedError, false,
                                    OuterDeps, Def->GeneratedHeader) ==
           c_autotmpl::InstantiationResult::Added);

    auto DeepBuffer = MemoryBuffer::getFile(DeepPath);
    assert(DeepBuffer);
    StringRef DeepContent = (*DeepBuffer)->getBuffer();
    assert(DeepContent.find("/* c-autotmpl: List(char) */") !=
           StringRef::npos);
    assert(DeepContent.find("/* c-autotmpl: Box(List_char) */") !=
           StringRef::npos);
    assert(DeepContent.find("/* c-autotmpl: List(Box_List_char) */") !=
           StringRef::npos);

    auto BoxBuffer = MemoryBuffer::getFile(BoxPath);
    assert(BoxBuffer);
    assert((*BoxBuffer)->getBuffer().find("/* c-autotmpl: Box(List_char) */") !=
           StringRef::npos);

    // Pruning must understand cross-template blocks: Box(List_char) was
    // flattened into gen/list.h along with the outer List instantiation.
    SmallVector<const c_autotmpl::TemplateDef *, 2> AllTemplates{Def, BoxDef};
    StringSet<> Keep;
    Keep.insert(c_autotmpl::instantiationMarker("List", "char"));
    Keep.insert(c_autotmpl::instantiationMarker("Box", "List_char"));
    std::vector<std::string> Removed;
    assert(Instantiator.prune(AllTemplates, Def->GeneratedHeader, Keep, Removed,
                              NestedError));
    bool RemovedOuter = false;
    for (const std::string &Marker : Removed)
      RemovedOuter |=
          Marker == c_autotmpl::instantiationMarker("List", "Box_List_char");
    assert(RemovedOuter);
    auto PrunedBuffer = MemoryBuffer::getFile(DeepPath);
    assert(PrunedBuffer);
    StringRef Pruned = (*PrunedBuffer)->getBuffer();
    assert(Pruned.find("/* c-autotmpl: List(Box_List_char) */") ==
           StringRef::npos);
    assert(Pruned.find("/* c-autotmpl: Box(List_char) */") != StringRef::npos);
  }

  {
    std::string OtherPath, OtherError;
    assert(Instantiator.instantiate(*SelectDef, "int, float", "",
                                    OtherPath, OtherError) ==
           c_autotmpl::InstantiationResult::Added);
    StringSet<> Keep;
    Keep.insert(c_autotmpl::instantiationMarker("select", "int, Foo"));
    std::vector<std::string> Removed;
    assert(Instantiator.prune(*SelectDef, Keep, Removed, OtherError));
    bool RemovedOther = false;
    for (const std::string &Marker : Removed)
      RemovedOther |=
          Marker == c_autotmpl::instantiationMarker("select", "int, float");
    assert(RemovedOther);
    auto PrunedBuffer = MemoryBuffer::getFile(OtherPath);
    assert(PrunedBuffer);
    StringRef Pruned = (*PrunedBuffer)->getBuffer();
    assert(Pruned.find("/* c-autotmpl: select(int, float) */") ==
           StringRef::npos);
    assert(Pruned.find("/* c-autotmpl: select(int, Foo) */") !=
           StringRef::npos);
  }

  {
    // dispatch_truncate=1: bare operation macros whose concrete targets are
    // pasted directly from the template arguments.
    c_autotmpl::setDispatchTruncate(true);
    c_autotmpl::TemplateRegistry TruncateRegistry;
    TruncateRegistry.scan(Root);
    const c_autotmpl::TemplateDef *DirectDef =
        TruncateRegistry.findByName("Direct");
    const c_autotmpl::TemplateDef *SlabDef =
        TruncateRegistry.findByName("Slab");
    assert(DirectDef && SlabDef);
    assert(DirectDef->DispatchOps.contains("direct_set"));
    assert(!DirectDef->DispatchOps.contains("direct_set_impl"));
    assert(SlabDef->DispatchOps.contains("slab_set"));
    assert(SlabDef->DispatchTargets.find("slab_set") !=
           SlabDef->DispatchTargets.end());
    auto DirectTarget = DirectDef->DispatchTargets.find("direct_set");
    assert(DirectTarget != DirectDef->DispatchTargets.end());
    assert(DirectTarget->second.Base == "direct_set" &&
           DirectTarget->second.AppendArgs);

    c_autotmpl::DispatchConfig TruncCfg;
    TruncCfg.Enabled = true;
    TruncCfg.Truncate = true;
    TruncateRegistry.applyDispatch(TruncCfg);
    SmallVector<c_autotmpl::DispatchCase, 1> Cases{
        {"int, float", "Direct_int_float", {"int", "float"}}};
    SmallVector<StringRef, 1> Ops{"direct_set"};
    std::string Section = c_autotmpl::makeDispatchSection(
        *DirectDef, Cases, Ops, TruncCfg);
    for (StringRef Needle : {
             "/* c-autotmpl dispatch begin Direct */",
             "/* c-autotmpl generated-by=plugin */",
             "Direct_int_float: direct_set_int_float",
             "Direct_int_float*: direct_set_int_float",
             "#define C_AUTOTMPL_DISPATCH_PICK_Direct_direct_set("})
      assert(Section.find(Needle.str()) != std::string::npos);
    assert(Section.find("#define direct_set(...)") == std::string::npos);
    assert(Section.find("#define $direct_set(...)") == std::string::npos);
    c_autotmpl::DispatchSharedGroup TruncateGroup;
    TruncateGroup.Key = "direct_set";
    TruncateGroup.SelectorOp = "direct_set";
    TruncateGroup.Contribs.push_back(
        {DirectDef, nullptr, copyCases(Cases), "direct_set"});
    std::string Shared = c_autotmpl::makeSharedOpSection(TruncateGroup,
                                                         TruncCfg);
    assert(Shared.find("#define direct_set(...)") != std::string::npos);
    assert(countOccurrences(Shared, "#define direct_set(") == 1);
    c_autotmpl::setDispatchTruncate(false);
  }

  {
    // Owner mangling is orthogonal to explicit method arguments: ordinary
    // operations and curried methods may both opt in with a section decorator.
    SmallString<128> OwnerRoot("/tmp/c-autotmpl-owner-selftest");
    sys::fs::remove_directories(OwnerRoot);
    sys::fs::create_directories(join(OwnerRoot, "tmpl"));
    {
      std::error_code EC;
      raw_fd_ostream OS(join(OwnerRoot, "tmpl/box.h"), EC);
      OS << "// -- template type Box(T)\n"
            "// -- +owner\n"
            "#define BOX_CAT2_I(A, B) A##B\n"
            "#define BOX_CAT2(A, B) BOX_CAT2_I(A, B)\n"
            "#define BOX_CAT3_I(A, B, C) A##_##B##_##C\n"
            "#define BOX_CAT3(A, B, C) BOX_CAT3_I(A, B, C)\n"
            "#define Box(T) BOX_CAT2(Box_, T)\n"
            "#define get_impl(T, Owner) BOX_CAT3(get, T, Owner)\n"
            "typedef struct { T value; } Box(T);\n"
            "static inline T get_impl(T, Owner)(Box(T) *b) { return b->value; }\n"
            "#undef get_impl\n";
    }
    {
      std::error_code EC;
      raw_fd_ostream OS(join(OwnerRoot, "tmpl/container.h"), EC);
      OS << "// -- template type Container(T)\n"
            "// -- mangle +owner\n"
            "#define CONTAINER_CAT2_I(A, B) A##B\n"
            "#define CONTAINER_CAT2(A, B) CONTAINER_CAT2_I(A, B)\n"
            "#define CONTAINER_CAT3_I(A, B, C) A##_##B##_##C\n"
            "#define CONTAINER_CAT3(A, B, C) CONTAINER_CAT3_I(A, B, C)\n"
            "#define Container(T) CONTAINER_CAT2(Container_, T)\n"
            "#define get_impl(T, Owner) CONTAINER_CAT3(get, T, Owner)\n"
            "typedef struct { T value; } Container(T);\n"
            "static inline T get_impl(T, Owner)(Container(T) *c) "
            "{ return c->value; }\n"
            "#undef get_impl\n";
    }
    {
      std::error_code EC;
      raw_fd_ostream OS(join(OwnerRoot, "tmpl/option.h"), EC);
      OS << "// -- template type Option(T)\n"
            "#define OPTION_CAT2_I(A, B) A##B\n"
            "#define OPTION_CAT2(A, B) OPTION_CAT2_I(A, B)\n"
            "#define OPTION_CAT4_I(A, B, C, D) A##_##B##_##C##_##D\n"
            "#define OPTION_CAT4(A, B, C, D) OPTION_CAT4_I(A, B, C, D)\n"
            "#define Option(T) OPTION_CAT2(Option_, T)\n"
            "typedef struct { T value; } Option(T);\n"
            "\n"
            "// -- template function option_map_impl(T, U, Owner)\n"
            "// -- dispatch method Option.map explicit U\n"
            "// -- +owner\n"
            "#define option_map_impl(T, U, Owner) "
            "OPTION_CAT4(map, T, U, Owner)\n"
            "static inline Option(U) option_map_impl(T, U, Owner)"
            "(Option(T) x, U (*f)(T)) {\n"
            "  Option(U) out = {f(x.value)};\n"
            "  return out;\n"
            "}\n";
    }
    {
      std::error_code EC;
      raw_fd_ostream OS(join(OwnerRoot, "tmpl/result.h"), EC);
      OS << "// -- template type Result(T, U)\n"
            "#define RESULT_CAT3_I(A, B, C) A##_##B##_##C\n"
            "#define RESULT_CAT3(A, B, C) RESULT_CAT3_I(A, B, C)\n"
            "#define RESULT_CAT5_I(A, B, C, D, E) A##_##B##_##C##_##D##_##E\n"
            "#define RESULT_CAT5(A, B, C, D, E) RESULT_CAT5_I(A, B, C, D, E)\n"
            "#define Result(T, U) RESULT_CAT3(Result, T, U)\n"
            "typedef struct { T value; U error; } Result(T, U);\n"
            "\n"
            "// -- template function result_map_impl(T, U, V, Owner)\n"
            "// -- dispatch method Result.map explicit V\n"
            "// -- mangle +owner\n"
            "#define result_map_impl(T, U, V, Owner) "
            "RESULT_CAT5(map, T, U, V, Owner)\n"
            "static inline Result(V, U) result_map_impl(T, U, V, Owner)"
            "(Result(T, U) x, V (*f)(T)) {\n"
            "  Result(V, U) out = {f(x.value), x.error};\n"
            "  return out;\n"
            "}\n";
    }

    c_autotmpl::TemplateRegistry OwnerRegistry;
    c_autotmpl::setDispatchTruncate(true);
    OwnerRegistry.scan(OwnerRoot);
    c_autotmpl::setDispatchTruncate(false);
    c_autotmpl::DispatchConfig OwnerCfg;
    OwnerCfg.Enabled = true;
    OwnerCfg.Truncate = true;
    OwnerRegistry.applyDispatch(OwnerCfg);

    const c_autotmpl::TemplateDef *BoxDef =
        OwnerRegistry.findByName("Box");
    const c_autotmpl::TemplateDef *ContainerDef =
        OwnerRegistry.findByName("Container");
    const c_autotmpl::TemplateDef *OptionDef =
        OwnerRegistry.findByName("Option");
    assert(BoxDef && ContainerDef && OptionDef);
    assert(BoxDef->MangleOwnerOps.contains("get"));
    assert(ContainerDef->MangleOwnerOps.contains("get"));
    assert(BoxDef->DispatchTargets.find("get")->second.MangleOwner);
    assert(ContainerDef->DispatchTargets.find("get")->second.MangleOwner);
    // A decorated method section does not qualify the type's ordinary ops.
    assert(OptionDef->MangleOwnerOps.empty());

    SmallVector<const c_autotmpl::DispatchMethodDef *, 2> MapMethods;
    OwnerRegistry.findDispatchMethods("map", MapMethods);
    assert(MapMethods.size() == 2);
    const c_autotmpl::DispatchMethodDef *OptionMap = nullptr;
    const c_autotmpl::DispatchMethodDef *ResultMap = nullptr;
    for (const c_autotmpl::DispatchMethodDef *Method : MapMethods) {
      assert(Method->MangleOwner);
      if (Method->Owner->Name == "Option") {
        OptionMap = Method;
        assert(Method->Impl.Params.size() == 3);
      } else {
        ResultMap = Method;
        assert(Method->Owner->Name == "Result");
        assert(Method->Impl.Params.size() == 4);
      }
    }
    assert(OptionMap && ResultMap);

    SmallVector<c_autotmpl::DispatchCase, 1> BoxCases{
        {"int", "Box_int", {"int"}}};
    SmallVector<c_autotmpl::DispatchCase, 1> ContainerCases{
        {"float", "Container_float", {"float"}}};
    SmallVector<StringRef, 1> GetOps{"get"};
    std::string BoxSection = c_autotmpl::makeDispatchSection(
        *BoxDef, BoxCases, GetOps, OwnerCfg);
    std::string ContainerSection = c_autotmpl::makeDispatchSection(
        *ContainerDef, ContainerCases, GetOps, OwnerCfg);
    assert(BoxSection.find("Box_int: get_int_Box") != std::string::npos);
    assert(ContainerSection.find("Container_float: get_float_Container") !=
           std::string::npos);
    assert(BoxSection.find("Box_int: get_int,") == std::string::npos);
    assert(ContainerSection.find("Container_float: get_float,") ==
           std::string::npos);

    SmallVector<c_autotmpl::DispatchCase, 1> OptionCases{
        {"int", "Option_int", {"int"}}};
    SmallVector<c_autotmpl::DispatchCase, 1> ResultCases{
        {"int, float", "Result_int_float", {"int", "float"}}};
    SmallVector<c_autotmpl::DispatchMethodUse, 1> IntUses{
        {"int", {"int"}}};
    std::string OptionMapSection = c_autotmpl::makeDispatchMethodSection(
        *OptionMap, OptionCases, IntUses, OwnerCfg, "");
    std::string ResultMapSection = c_autotmpl::makeDispatchMethodSection(
        *ResultMap, ResultCases, IntUses, OwnerCfg, "");
    assert(OptionMapSection.find("Option_int: map_int_int_Option") !=
           std::string::npos);
    assert(ResultMapSection.find("Result_int_float: map_int_float_int_Result") !=
           std::string::npos);

    std::string OptionImpl = c_autotmpl::makeImplInstantiationBlock(
        OptionMap->Impl, "int, int, Option", {"int", "int", "Option"}, {});
    assert(OptionImpl.find("#define Owner Option\n") != std::string::npos);
    assert(OptionImpl.find("#undef Owner\n") != std::string::npos);
    assert(countOccurrences(OptionImpl, "#define Owner ") == 1);
  }

  std::cout << "selftest OK\n";
  return 0;
}
