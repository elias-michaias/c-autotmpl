//===--- AutotmplModule.cpp - clangd feature module for C templates -------===//
//
// Dynamically loaded clangd feature module. It wraps didOpen/didChange to scan
// unsaved buffers for `List(int)` / `List_int` usages and write missing
// instantiation blocks before clangd builds a preamble. Stale blocks are pruned
// from a cached, mtime-aware index of workspace sources.
//
// The editor only has to pass `--load=libc-autotmpl.so`; root discovery,
// generation, guard handling and pruning are all implemented here.

// LSPBinder keeps RawHandlers private, but a feature module must wrap clangd's
// didOpen/didChange notifications to update generated headers before clangd
// builds a preamble. Pre-include its dependencies, then expose the members for
// this translation unit only.
#include "Protocol.h"
#include "support/Function.h"
#include "support/Logger.h"
#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/JSON.h"
#define private public
#include "LSPBinder.h"
#undef private

#include "FeatureModule.h"
#include "Diagnostics.h"
#include "SourceCode.h"

#include "Instantiator.h"
#include "SourceScan.h"
#include "TemplateRegistry.h"
#include "TypeIndex.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclarationName.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Preprocessor.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace clang;
using namespace clang::clangd;
using namespace llvm;

namespace c_autotmpl {
namespace {

constexpr llvm::StringLiteral LogPrefix("[c-autotmpl] ");

void log(StringRef Message) {
  errs() << LogPrefix << Message << '\n';
}

bool timingEnabled() {
  static const bool Enabled =
      sys::Process::GetEnv("C_AUTOTMPL_TIMING").has_value();
  return Enabled;
}

bool isIdentChar(char C) {
  return C == '_' || (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
         (C >= '0' && C <= '9');
}

bool isSpace(char C) {
  return C == ' ' || C == '\t' || C == '\r' || C == '\n' || C == '\f' ||
         C == '\v';
}

StringRef stripTypeQualifiers(StringRef S) {
  S = S.trim();
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (StringRef Q : {"const", "volatile", "restrict", "struct", "union",
                        "enum", "class"}) {
      if (!S.starts_with(Q))
        continue;
      if (S.size() != Q.size() && !isSpace(S[Q.size()]))
        continue;
      S = S.drop_front(Q.size()).ltrim();
      Changed = true;
    }
  }
  return S;
}

bool isHeaderExtension(StringRef Ext) {
  return Ext == ".h" || Ext == ".hh" || Ext == ".hpp" || Ext == ".hxx";
}

bool isSourceExtension(StringRef Ext) {
  return isHeaderExtension(Ext) || Ext == ".c" || Ext == ".cc" ||
         Ext == ".cpp" || Ext == ".cxx";
}

bool skipDirectory(StringRef Name) {
  return Name == "build" || Name == "node_modules" || Name == "gen" ||
         Name == ".git" || (!Name.empty() && Name.front() == '.');
}

SmallString<256> joinPath(StringRef Dir, StringRef Name) {
  SmallString<256> Out(Dir);
  sys::path::append(Out, Name);
  return Out;
}

bool hasTemplateDecorator(StringRef Dir) {
  std::error_code EC;
  for (sys::fs::directory_iterator It(Dir, EC), End; It != End && !EC;
       It.increment(EC)) {
    if (It->type() != sys::fs::file_type::regular_file)
      continue;
    if (!isHeaderExtension(sys::path::extension(It->path())))
      continue;
    auto Buf = MemoryBuffer::getFile(It->path(), /*IsText=*/true);
    if (!Buf)
      continue;
    StringRef Rest = (*Buf)->getBuffer();
    for (int I = 0; I < 80 && !Rest.empty(); ++I) {
      StringRef Line;
      std::tie(Line, Rest) = Rest.split('\n');
      if (Line.contains("// -- template"))
        return true;
    }
  }
  return false;
}

/// Finds the workspace root with no editor configuration. A directory holding
/// decorated templates wins over build markers, so a nested `.clangd` with only
/// compile flags cannot hide the real root. Env override is kept for unusual
/// layouts.
SmallString<256> findRoot(StringRef MainFile) {
  if (auto Env = sys::Process::GetEnv("C_AUTOTMPL_ROOT"))
    return SmallString<256>(makeAbsolute(*Env));

  SmallString<256> Dir(sys::path::parent_path(MainFile));
  if (Dir.empty())
    Dir = ".";

  // Two passes: a nested .clangd must not hide templates in an ancestor.
  for (int Pass = 0; Pass < 2; ++Pass) {
    SmallString<256> Current = Dir;
    for (int I = 0; I < 8 && !Current.empty(); ++I) {
      if (Pass == 0) {
        for (StringRef Name : {"tmpl", "templates"}) {
          SmallString<256> Candid = joinPath(Current, Name);
          if (sys::fs::is_directory(Candid) && hasTemplateDecorator(Candid))
            return SmallString<256>(makeAbsolute(Current));
        }
      } else if (sys::fs::exists(joinPath(Current, ".clangd")) ||
                 sys::fs::exists(joinPath(Current, "compile_commands.json")) ||
                 sys::fs::exists(joinPath(Current, ".git"))) {
        return SmallString<256>(makeAbsolute(Current));
      }

      SmallString<256> Parent(sys::path::parent_path(Current));
      if (Parent == Current)
        break;
      Current = Parent;
    }
  }
  return SmallString<256>(makeAbsolute(sys::path::parent_path(MainFile)));
}

SmallString<64> argString(const clang::Diagnostic &Info, unsigned Idx) {
  if (Info.getNumArgs() <= Idx)
    return {};
  switch (Info.getArgKind(Idx)) {
  case clang::DiagnosticsEngine::ak_identifierinfo:
    return SmallString<64>(Info.getArgIdentifier(Idx)->getName());
  case clang::DiagnosticsEngine::ak_std_string:
    return SmallString<64>(Info.getArgStdStr(Idx));
  case clang::DiagnosticsEngine::ak_c_string:
    return SmallString<64>(Info.getArgCStr(Idx));
  case clang::DiagnosticsEngine::ak_declarationname:
    return SmallString<64>(
        DeclarationName::getFromOpaqueInteger(Info.getRawArg(Idx))
            .getAsString());
  default:
    return {};
  }
}

bool isTemplateDiag(unsigned ID) {
  return ID == diag::err_undeclared_var_use ||
         ID == diag::err_undeclared_var_use_suggest ||
         ID == diag::err_unknown_typename ||
         ID == diag::err_unknown_typename_suggest ||
         ID == diag::err_unknown_type_or_class_name_suggest;
}

/// Extracts the type as written at the diagnostic location. For macro uses
/// like `List(float)` this returns "float"; otherwise falls back to the
/// suffix of the mangled name.
StringRef typeFromSource(const clang::SourceManager &SM, SourceLocation Loc,
                         StringRef Fallback) {
  SourceLocation Expansion = SM.getExpansionLoc(Loc);
  if (Expansion.isInvalid())
    return Fallback;

  bool Invalid = false;
  StringRef Buffer = SM.getBufferData(SM.getFileID(Expansion), &Invalid);
  if (Invalid)
    return Fallback;

  unsigned Offset = SM.getFileOffset(Expansion);
  if (Offset >= Buffer.size())
    return Fallback;

  StringRef Rest = Buffer.drop_front(Offset);
  size_t I = 0;
  while (I < Rest.size() && isIdentChar(Rest[I]))
    ++I;
  while (I < Rest.size() && isSpace(Rest[I]))
    ++I;
  if (I >= Rest.size() || Rest[I] != '(')
    return Fallback;
  ++I;
  while (I < Rest.size() && isSpace(Rest[I]))
    ++I;

  size_t Start = I;
  int Depth = 1;
  for (; I < Rest.size(); ++I) {
    if (Rest[I] == '(')
      ++Depth;
    else if (Rest[I] == ')') {
      if (--Depth == 0)
        break;
    }
  }
  if (Depth != 0)
    return Fallback;
  StringRef Type = Rest.slice(Start, I).trim();
  return Type.empty() ? Fallback : Type;
}

/// AST fallback for a type guard when the text index has no declaration.
/// All candidates are tried so a forward declaration in an unguarded header
/// cannot hide a later guarded declaration.
SmallVector<std::string, 4> definingHeaderPaths(clang::CompilerInstance &CI,
                                                StringRef Spelling) {
  SmallVector<std::string, 4> Paths;
  StringRef Name = lastIdentifier(Spelling);
  if (Name.empty() || isBuiltinTypeName(Name))
    return Paths;

  ASTContext &Ctx = CI.getASTContext();
  const IdentifierInfo *II =
      &CI.getPreprocessor().getIdentifierTable().get(Name);
  if (!II)
    return Paths;

  SourceManager &SM = Ctx.getSourceManager();
  for (NamedDecl *D : Ctx.getTranslationUnitDecl()->lookup(II)) {
    SourceLocation Loc = D->getLocation();
    if (Loc.isInvalid() || SM.isInMainFile(Loc))
      continue;
    auto FE = SM.getFileEntryRefForID(SM.getFileID(Loc));
    if (!FE)
      continue;
    StringRef Path = FE->getFileEntry().tryGetRealPathName();
    if (Path.empty())
      Path = FE->getName();
    Paths.push_back(makeAbsolute(Path));
  }
  return Paths;
}

struct UsageFile {
  uint64_t Stamp = 0;
  // Generated-header path -> instantiation markers used by this file.
  llvm::StringMap<llvm::StringSet<>> Wanted;
};

/// Shared state, surviving across AST builds and listener instances.
struct SharedState {
  TemplateRegistry Registry;
  TypeIndex Types;
  Instantiator Inst;

  std::mutex ScanMutex;
  std::atomic<bool> Scanned{false};

  std::mutex TypesMutex;
  // Every guarded type name seen so far. The index is rebuilt only when a new
  // name appears, so types introduced by later open files are still covered.
  llvm::StringSet<> TypeNamesRequested;

  std::mutex GuardMutex;
  llvm::StringMap<std::string> GuardCache;
  llvm::StringSet<> Warned;
  llvm::StringSet<> GeneratedHeaders;

  std::mutex UsageMutex;
  std::string Root;
  llvm::StringMap<UsageFile> UsageCache;
  llvm::StringMap<llvm::StringSet<>> Wanted;
  bool UsageScanned = false;
  std::chrono::steady_clock::time_point LastUsageScan;
  // Latest unsaved text from LSP notifications, keyed by absolute path.
  llvm::StringMap<std::string> OpenFiles;
  bool WorkspaceScanStarted = false;
  bool PruningReady = false;
  std::thread WorkspaceThread;

  ~SharedState() {
    if (WorkspaceThread.joinable())
      WorkspaceThread.join();
  }

  void ensureScanned(StringRef MainFile) {
    if (Scanned.load(std::memory_order_acquire))
      return;
    std::lock_guard<std::mutex> Lock(ScanMutex);
    if (Scanned.load(std::memory_order_relaxed))
      return;

    auto Start = std::chrono::steady_clock::now();

    Root = findRoot(MainFile).str();
    Registry.scanPreferTemplateDirs(Root);
    // Type guards are indexed lazily from the types actually used in the open
    // buffer. Scanning every unrelated header here would delay first parse.
    for (const auto &Def : Registry.templates())
      GeneratedHeaders.insert(Def->GeneratedHeader);

    Scanned.store(true, std::memory_order_release);
    std::string Msg = "scanning " + Root + ", found " +
                      std::to_string(Registry.templates().size()) +
                      " template(s)";
    if (timingEnabled())
      Msg += " in " +
             std::to_string(std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - Start)
                                .count()) +
             " ms";
    log(Msg);
  }

  /// Builds only the part of the type index needed by this buffer. Deferring
  /// this until a user-defined type is actually seen keeps the first parse
  /// cheap in projects with many unrelated headers.
  void ensureTypesScanned(StringRef Text) {
    SmallVector<FoundUsage, 32> Usages;
    scanUsages(Text, Registry, Usages);

    std::lock_guard<std::mutex> Lock(TypesMutex);
    bool Added = false;
    for (const FoundUsage &Usage : Usages) {
      if (!needsIncludeGuard(Usage.TypeSpelling) ||
          isGeneratedType(Usage.TypeSpelling))
        continue;
      StringRef Name = lastIdentifier(Usage.TypeSpelling);
      if (!Name.empty())
        Added |= TypeNamesRequested.insert(Name.str()).second;
    }
    if (!Added)
      return;

    SmallVector<std::string, 16> Names;
    for (const auto &Entry : TypeNamesRequested)
      Names.push_back(Entry.getKey().str());
    Types.scan(Root, Names);
  }

  std::string guardForPath(StringRef Path) {
    if (Path.empty())
      return {};
    std::lock_guard<std::mutex> Lock(GuardMutex);
    auto It = GuardCache.find(Path);
    if (It != GuardCache.end())
      return It->second;
    std::string Guard = findIncludeGuard(Path);
    GuardCache[Path] = Guard;
    return Guard;
  }

  bool warnOnce(StringRef Key) {
    std::lock_guard<std::mutex> Lock(GuardMutex);
    return Warned.insert(Key).second;
  }

  static uint64_t stampFor(StringRef Path) {
    sys::fs::file_status Status;
    if (sys::fs::status(Path, Status))
      return 0;
    uint64_t Time = static_cast<uint64_t>(
        Status.getLastModificationTime().time_since_epoch().count());
    return (Time << 1) ^ static_cast<uint64_t>(Status.getSize());
  }

  void collectUsages(StringRef Text, StringRef Path,
                     llvm::StringMap<llvm::StringSet<>> &Out) const {
    SmallVector<FoundUsage, 32> Usages;
    scanUsages(Text, Registry, Usages);
    for (size_t I = 0; I < Usages.size(); ++I) {
      if (Usages[I].IsNested)
        continue;
      size_t Begin = I;
      while (Begin > 0 && Usages[Begin - 1].IsNested)
        --Begin;
      StringRef OutputHeader = Usages[I].Template->GeneratedHeader;
      for (size_t J = Begin; J <= I; ++J) {
        const FoundUsage &Usage = Usages[J];
        std::string Marker =
            instantiationMarker(Usage.Template->Name, Usage.TypeSpelling);
        Out[Usage.Template->GeneratedHeader].insert(Marker);
        // A nested chain is also flattened into the outermost header, so that
        // header owns these markers for pruning too.
        Out[OutputHeader].insert(std::move(Marker));
      }
    }
    (void)Path;
  }

  // Requires UsageMutex.
  void rebuildWanted() {
    Wanted.clear();
    for (const auto &File : UsageCache)
      for (const auto &Entry : File.second.Wanted)
        for (const auto &Marker : Entry.second)
          Wanted[Entry.getKey()].insert(Marker.getKey());
  }

  // Requires UsageMutex. Open files use their unsaved buffers; other files are
  // read only when their mtime/size changed.
  void updateUsageLocked(StringRef Path) {
    auto Open = OpenFiles.find(Path);
    bool IsOpen = Open != OpenFiles.end();
    uint64_t Stamp = stampFor(Path);
    if (!IsOpen) {
      auto Cached = UsageCache.find(Path);
      if (Cached != UsageCache.end() && Cached->second.Stamp == Stamp)
        return;
    }

    StringRef Text;
    std::string DiskText;
    if (IsOpen) {
      Text = Open->second;
    } else {
      auto Buf = MemoryBuffer::getFile(Path, /*IsText=*/true);
      if (!Buf)
        return;
      DiskText = (*Buf)->getBuffer().str();
      Text = DiskText;
    }

    UsageFile Entry;
    Entry.Stamp = Stamp;
    collectUsages(Text, Path, Entry.Wanted);
    UsageCache[Path] = std::move(Entry);
  }

  // Requires UsageMutex.
  void scanWorkspaceLocked() {
    std::error_code EC;
    llvm::StringSet<> Seen;

    for (sys::fs::recursive_directory_iterator It(Root, EC,
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
      if (!isSourceExtension(sys::path::extension(It->path())))
        continue;

      std::string Path = makeAbsolute(It->path());
      if (GeneratedHeaders.contains(Path))
        continue;
      Seen.insert(Path);
      updateUsageLocked(Path);
    }

    SmallVector<std::string, 8> Stale;
    for (const auto &Entry : UsageCache)
      if (!Seen.contains(Entry.getKey()))
        Stale.push_back(Entry.getKey().str());
    for (const std::string &Path : Stale)
      UsageCache.erase(Path);

    rebuildWanted();
    UsageScanned = true;
    LastUsageScan = std::chrono::steady_clock::now();
  }

  /// Per-keystroke updates touch only the current file. The full workspace is
  /// walked once and then periodically to catch external edits and deletions.
  void refreshUsages() {
    std::lock_guard<std::mutex> Lock(UsageMutex);
    auto Now = std::chrono::steady_clock::now();
    if (!UsageScanned ||
        Now - LastUsageScan > std::chrono::seconds(2)) {
      scanWorkspaceLocked();
      return;
    }
    // Open files are few; re-scanning their buffers is cheap and keeps
    // multi-buffer editing correct.
    for (auto &Entry : OpenFiles)
      updateUsageLocked(Entry.getKey());
    rebuildWanted();
  }

  /// The workspace walk is only needed for pruning. Running it off the LSP
  /// thread keeps didOpen/didChange from waiting on unrelated headers.
  void startWorkspaceScan() {
    bool Start = false;
    {
      std::lock_guard<std::mutex> Lock(UsageMutex);
      if (!WorkspaceScanStarted) {
        WorkspaceScanStarted = true;
        Start = true;
      }
    }
    if (!Start)
      return;

    WorkspaceThread = std::thread([this] {
      auto Begin = std::chrono::steady_clock::now();
      {
        std::lock_guard<std::mutex> Lock(UsageMutex);
        scanWorkspaceLocked();
        PruningReady = true;
      }
      log("workspace scan complete");
      if (timingEnabled()) {
        double Ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - Begin)
                        .count();
        log("workspace scan in " + std::to_string(Ms) + " ms");
      }
    });
  }

  std::string openText(StringRef Path) {
    std::lock_guard<std::mutex> Lock(UsageMutex);
    auto It = OpenFiles.find(Path);
    return It == OpenFiles.end() ? std::string() : It->second;
  }

  void closeFile(StringRef Path) {
    std::lock_guard<std::mutex> Lock(UsageMutex);
    OpenFiles.erase(Path);
  }

  /// Called from clangd's didOpen/didChange notifications, before any parse.
  /// Writing here keeps the preamble stat cache consistent on the first build.
  void prepareUpdate(StringRef Path, StringRef Text) {
    if (Path.empty())
      return;
    auto Start = std::chrono::steady_clock::now();
    ensureScanned(Path);
    if (Registry.templates().empty())
      return;

    auto AfterScan = std::chrono::steady_clock::now();
    bool ReadyForPrune = false;
    {
      std::lock_guard<std::mutex> Lock(UsageMutex);
      ReadyForPrune = PruningReady;
      OpenFiles[Path] = Text.str();
      updateUsageLocked(Path);
      rebuildWanted();
    }
    startWorkspaceScan();
    ensureTypesScanned(Text);
    auto AfterUsages = std::chrono::steady_clock::now();

    if (ReadyForPrune)
      pruneAll();
    auto AfterPrune = std::chrono::steady_clock::now();
    generateFromText(Text, nullptr);
    auto End = std::chrono::steady_clock::now();

    if (timingEnabled()) {
      auto ms = [](std::chrono::steady_clock::time_point A,
                   std::chrono::steady_clock::time_point B) {
        return std::chrono::duration<double, std::milli>(B - A).count();
      };
      log("prepareUpdate in " + std::to_string(ms(Start, End)) +
          " ms (scan " + std::to_string(ms(Start, AfterScan)) +
          ", usages " + std::to_string(ms(AfterScan, AfterUsages)) +
          ", prune " + std::to_string(ms(AfterUsages, AfterPrune)) +
          ", generate " + std::to_string(ms(AfterPrune, End)) + ")");
    }
  }

  void pruneAll() {
    static const llvm::StringSet<> Empty;
    llvm::StringMap<llvm::StringSet<>> Snapshot;
    {
      std::lock_guard<std::mutex> Lock(UsageMutex);
      if (!PruningReady)
        return;
      Snapshot = Wanted;
    }

    SmallVector<const TemplateDef *, 8> Templates;
    for (const auto &Owned : Registry.templates())
      Templates.push_back(Owned.get());

    std::vector<std::string> Removed;
    std::string Error;
    for (const auto &Entry : GeneratedHeaders) {
      StringRef Header = Entry.getKey();
      Removed.clear();
      Error.clear();
      auto It = Snapshot.find(Header);
      const llvm::StringSet<> &Keep =
          It == Snapshot.end() ? Empty : It->second;
      if (!Inst.prune(Templates, Header, Keep, Removed, Error)) {
        if (!Error.empty())
          log(std::string("prune failed: ") + Error);
        continue;
      }
      for (const std::string &Marker : Removed) {
        log("removed " + Marker + " from " + Header.str());
      }
    }
  }

  void generateFromText(StringRef Text, clang::CompilerInstance *CI) {
    SmallVector<FoundUsage, 32> Usages;
    scanUsages(Text, Registry, Usages);
    generateUsages(Usages, CI);
  }

  /// Groups each top-level usage with the nested usages emitted immediately
  /// before it. The whole chain is written to one generated header so mutually
  /// dependent types cannot be separated by include order.
  void generateUsages(ArrayRef<FoundUsage> Usages,
                      clang::CompilerInstance *CI) {
    for (size_t I = 0; I < Usages.size(); ++I) {
      if (Usages[I].IsNested)
        continue;
      size_t Begin = I;
      while (Begin > 0 && Usages[Begin - 1].IsNested)
        --Begin;
      SmallVector<const FoundUsage *, 8> Chain;
      for (size_t J = Begin; J <= I; ++J)
        Chain.push_back(&Usages[J]);
      generateChain(Chain, CI);
    }
  }

  bool generateChain(ArrayRef<const FoundUsage *> Chain,
                     clang::CompilerInstance *CI,
                     std::string *GenPathOut = nullptr) {
    if (Chain.empty())
      return false;
    const std::string OutputHeader = Chain.back()->Template->GeneratedHeader;
    if (GenPathOut)
      *GenPathOut = OutputHeader;
    bool Added = false;
    for (const FoundUsage *Usage : Chain) {
      Added |= generateInto(*Usage, OutputHeader, CI);
      // Keep the dependency's own generated header up to date as well. The
      // presence macro makes the two copies harmless, and it guarantees the
      // header exists for sources that include it before this one.
      if (Usage != Chain.back() &&
          Usage->Template->GeneratedHeader != OutputHeader)
        Added |= generateInto(*Usage, Usage->Template->GeneratedHeader, CI);
    }
    return Added;
  }

  /// Writes one usage block into OutputHeader, which may belong to an outer
  /// template when this is a nested dependency.
  bool generateInto(const FoundUsage &Usage, StringRef OutputHeader,
                    clang::CompilerInstance *CI) {
    if (Inst.has(*Usage.Template, Usage.TypeSpelling, OutputHeader))
      return false;

    SmallVector<DependencyGuard, 2> Guards;
    dependencyGuards(Usage, OutputHeader, Guards);

    std::string Guard = guardForType(Usage.TypeSpelling, CI);
    if (needsIncludeGuard(Usage.TypeSpelling) &&
        !isGeneratedType(Usage.TypeSpelling) && Guard.empty()) {
      warnNoGuardOnce(Usage.Template->Name, Usage.TypeSpelling);
      return false;
    }

    std::string GenPath, Error;
    InstantiationResult Result =
        Inst.instantiate(*Usage.Template, Usage.TypeSpelling, Guard, GenPath,
                         Error, /*DryRun=*/false, Guards, OutputHeader);
    if (Result == InstantiationResult::Error) {
      log(std::string("failed: ") + Error);
      return false;
    }
    if (Result == InstantiationResult::Added) {
      log(std::string("generated ") + Usage.Template->Name + "(" +
          Usage.TypeSpelling + ") in " + GenPath);
      return true;
    }
    return false;
  }

  bool isGeneratedType(StringRef Spelling) const {
    StringRef Type = stripTypeQualifiers(Spelling);
    StringRef Arg;
    return Registry.matchMangled(Type, Arg) != nullptr;
  }

  std::string guardForType(StringRef Spelling, clang::CompilerInstance *CI) {
    return guardForTypeImpl(Spelling, CI, 0);
  }

  std::string guardForTypeImpl(StringRef Spelling, clang::CompilerInstance *CI,
                               unsigned Depth) {
    if (Depth > 16 || !needsIncludeGuard(Spelling))
      return {};

    StringRef Type = stripTypeQualifiers(Spelling);
    StringRef Arg;
    if (Registry.matchMangled(Type, Arg)) {
      // A generated template instantiation needs whatever guard its type
      // arguments need, e.g. List(Box(Foo)) is guarded by FOO_H.
      return guardForTypeImpl(Arg, CI, Depth + 1);
    }

    std::string Guard;
    {
      std::lock_guard<std::mutex> Lock(TypesMutex);
      Guard = Types.guardForType(Type);
    }
    if (!Guard.empty())
      return Guard;

    if (CI) {
      for (const std::string &Header : definingHeaderPaths(*CI, Type)) {
        Guard = guardForPath(Header);
        if (!Guard.empty())
          return Guard;
      }
    }
    return {};
  }

  void dependencyGuards(const FoundUsage &Usage, StringRef OutputHeader,
                        SmallVectorImpl<DependencyGuard> &Guards) {
    llvm::StringSet<> Seen;
    for (const UsageDep &Dep : Usage.DependsOn) {
      // Dependencies in the same generated header are emitted in this chain
      // before the block that needs them, so they need no cross-header guard.
      if (Dep.Template->GeneratedHeader == OutputHeader)
        continue;
      std::string Guard =
          generatedHeaderFileGuard(Dep.Template->GeneratedHeader);
      if (!Guard.empty() && Seen.insert(Guard).second)
        Guards.push_back({std::move(Guard)});
    }
  }

  bool warnNoGuardOnce(StringRef TemplateName, StringRef TypeSpelling) {
    std::string Key =
        "no-guard:" + TemplateName.str() + "(" + TypeSpelling.str() + ")";
    if (!warnOnce(Key))
      return false;
    log("skipping " + TemplateName.str() + "(" + TypeSpelling.str() +
        "): no traditional include guard found for type");
    return true;
  }
};

class Listener final : public FeatureModule::ASTListener {
public:
  explicit Listener(std::shared_ptr<SharedState> State)
      : State(std::move(State)) {}

  void beforeExecute(clang::CompilerInstance &CI) override {
    this->CI = &CI;
    const SourceManager &SM = CI.getSourceManager();
    if (auto FE = SM.getFileEntryRefForID(SM.getMainFileID())) {
      MainFile = FE->getFileEntry().tryGetRealPathName().str();
      if (MainFile.empty())
        MainFile = FE->getName().str();
      MainFile = makeAbsolute(MainFile);
    }
    // Generation and pruning happen in didOpen/didChange, before clangd builds
    // the preamble. Doing it here would leave the preamble stat cache stale.
    State->ensureScanned(MainFile);
  }

  void sawDiagnostic(const clang::Diagnostic &Info, Diag &D) override {
    if (!CI || !isTemplateDiag(Info.getID()))
      return;

    SmallString<64> MangledStorage = argString(Info, 0);
    StringRef Mangled = MangledStorage;
    if (Mangled.empty())
      return;

    StringRef TypeSpelling;
    const TemplateDef *Def =
        State->Registry.matchMangled(Mangled, TypeSpelling);
    if (!Def)
      return;

    // Prefer the spelling written in the source: `List(float)` rather than
    // the flattened `List_float` name.
    const SourceManager &SM = CI->getSourceManager();
    TypeSpelling = typeFromSource(SM, Info.getLocation(), TypeSpelling);
    if (TypeSpelling.empty())
      return;

    // The source may spell a nested type such as `List(Box(int))`. Expand it
    // into inner-first usages so dependencies are generated the same way as
    // the didOpen/didChange path.
    SmallVector<FoundUsage, 4> Usages;
    scanTemplateUsage(*Def, TypeSpelling, State->Registry, Usages);
    if (Usages.empty())
      return;

    SmallVector<const FoundUsage *, 4> Chain;
    for (const FoundUsage &Usage : Usages)
      Chain.push_back(&Usage);
    const FoundUsage &Outer = Usages.back();

    std::string GenPath;
    State->generateChain(Chain, CI, &GenPath);
    addIncludeFix(SM, Outer.Template->Name, GenPath, D);
  }

private:
  void addIncludeFix(const clang::SourceManager &SM, StringRef TemplateName,
                     StringRef GenPath, Diag &D) {
    if (MainFile.empty() || AddedInclude)
      return;

    std::string Rel =
        computeRelativePath(sys::path::parent_path(MainFile), GenPath);
    bool Invalid = false;
    StringRef Buffer = SM.getBufferData(SM.getMainFileID(), &Invalid);
    if (Invalid || Buffer.contains(Rel)) {
      AddedInclude = true;
      return;
    }

    TextEdit Edit;
    Edit.range.start = Position{0, 0};
    Edit.range.end = Position{0, 0};
    Edit.newText = "#include \"" + Rel + "\"\n";
    SmallVector<TextEdit, 1> Edits;
    Edits.push_back(std::move(Edit));
    D.Fixes.push_back(Fix{
        std::string("include generated header for ") + TemplateName.str(),
        std::move(Edits), {}});
    AddedInclude = true;
  }

  std::shared_ptr<SharedState> State;
  clang::CompilerInstance *CI = nullptr;
  std::string MainFile;
  bool AddedInclude = false;
};

class AutotmplModule final : public FeatureModule {
public:
  std::unique_ptr<ASTListener> astListeners() override {
    return std::make_unique<Listener>(State);
  }

  void initializeLSP(LSPBinder &Bind, const llvm::json::Object &,
                     llvm::json::Object &) override {
    wrapNotification(Bind, "textDocument/didOpen",
                     &AutotmplModule::onDidOpen);
    wrapNotification(Bind, "textDocument/didChange",
                     &AutotmplModule::onDidChange);
    wrapNotification(Bind, "textDocument/didClose",
                     &AutotmplModule::onDidClose);
  }

private:
  using Notification = llvm::unique_function<void(llvm::json::Value)>;

  void wrapNotification(LSPBinder &Bind, llvm::StringLiteral Method,
                        void (AutotmplModule::*Handler)(llvm::json::Value &)) {
    auto It = Bind.Raw.NotificationHandlers.find(Method);
    if (It == Bind.Raw.NotificationHandlers.end())
      return;
    Notification Original = std::move(It->second);
    Bind.Raw.NotificationHandlers[Method] =
        [this, Handler, Original = std::move(Original)](
            llvm::json::Value Raw) mutable {
          (this->*Handler)(Raw);
          if (Original)
            Original(std::move(Raw));
        };
  }

  void onDidOpen(llvm::json::Value &Raw) {
    DidOpenTextDocumentParams Params;
    llvm::json::Path::Root Root;
    if (!fromJSON(Raw, Params, Root))
      return;
    State->prepareUpdate(Params.textDocument.uri.file(),
                         Params.textDocument.text);
  }

  void onDidChange(llvm::json::Value &Raw) {
    DidChangeTextDocumentParams Params;
    llvm::json::Path::Root Root;
    if (!fromJSON(Raw, Params, Root))
      return;

    std::string Path = Params.textDocument.uri.file().str();
    std::string Text = State->openText(Path);
    for (const TextDocumentContentChangeEvent &Change : Params.contentChanges) {
      if (!Change.range) {
        Text = Change.text;
        continue;
      }
      auto Start = positionToOffset(Text, Change.range->start);
      auto End = positionToOffset(Text, Change.range->end);
      if (!Start || !End)
        continue;
      if (*Start > *End || *End > Text.size())
        continue;
      Text.replace(*Start, *End - *Start, Change.text);
    }
    State->prepareUpdate(Path, Text);
  }

  void onDidClose(llvm::json::Value &Raw) {
    DidCloseTextDocumentParams Params;
    llvm::json::Path::Root Root;
    if (fromJSON(Raw, Params, Root))
      State->closeFile(Params.textDocument.uri.file());
  }

  std::shared_ptr<SharedState> State = std::make_shared<SharedState>();
};

static FeatureModuleRegistry::Add<AutotmplModule>
    X("c-autotmpl", "Generate and prune re-include template instantiations");

} // namespace
} // namespace c_autotmpl
