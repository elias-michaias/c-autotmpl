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

using namespace llvm;

namespace {
std::string join(StringRef Base, StringRef Tail) {
  SmallString<128> P(Base);
  sys::path::append(P, Tail);
  return std::string(P);
}
} // namespace

int main() {
  SmallString<128> Root("/tmp/c-autotmpl-selftest");
  sys::fs::remove_directories(Root);
  sys::fs::create_directories(join(Root, "tmpl"));
  sys::fs::create_directories(join(Root, "other"));

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

  const c_autotmpl::TemplateDef *Def = Registry.findByName("List");
  assert(Def && Def->Name == "List");
  assert(Def->IncludeFromGenDir == "../tmpl/list.h");

  const c_autotmpl::TemplateDef *MapDef = Registry.findByName("Map");
  assert(MapDef && MapDef->Params.size() == 1 && MapDef->Params[0] == "Elem");

  StringRef Type;
  assert(Registry.matchMangled("List_int", Type) == Def && Type == "int");
  assert(Registry.matchMangled("Nope_int", Type) == nullptr);

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
  assert(Content.find("#ifndef C_AUTOTMPL_LIST_STRUCT_FOO_") !=
         StringRef::npos);
  assert(Content.find("#ifdef FOO_H") != StringRef::npos);
  assert(Content.find("#define T struct Foo") != StringRef::npos);

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
    assert(Nested.find("#ifndef C_AUTOTMPL_LIST_BOX_INT_") !=
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

  std::cout << "selftest OK\n";
  return 0;
}
