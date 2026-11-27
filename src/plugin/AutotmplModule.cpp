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
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
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

struct StcConfig {
  TemplatePreset Preset = TemplatePreset::None;
  std::string Name;
  std::vector<std::string> Templates;
  bool Auto = false;
};

static void addUniqueName(std::vector<std::string> &Out, StringRef Name) {
  Name = Name.trim();
  if (Name.empty())
    return;
  if (std::find(Out.begin(), Out.end(), Name.str()) == Out.end())
    Out.push_back(Name.str());
}

static bool parseBoolName(StringRef Text, bool &Out) {
  Text = Text.trim();
  if (Text == "1" || Text.equals_insensitive("true") || Text.equals_insensitive("yes") ||
      Text.equals_insensitive("on")) {
    Out = true;
    return true;
  }
  if (Text == "0" || Text.equals_insensitive("false") || Text.equals_insensitive("no") ||
      Text.equals_insensitive("off")) {
    Out = false;
    return true;
  }
  return false;
}

static void addNameList(StringRef Text, std::vector<std::string> &Out) {
  size_t I = 0;
  while (I < Text.size()) {
    while (I < Text.size() &&
           (Text[I] == ',' || StringRef(Text.substr(I, 1)).trim().empty()))
      ++I;
    size_t Begin = I;
    while (I < Text.size() && Text[I] != ',' &&
           !StringRef(Text.substr(I, 1)).trim().empty())
      ++I;
    if (I > Begin)
      addUniqueName(Out, Text.substr(Begin, I - Begin));
  }
}

/// Configuration comes from autotmpl.conf at the workspace root, then from
/// C_AUTOTMPL_* environment variables. An absent or unknown value leaves the
/// original behavior unchanged.
StcConfig resolvePreset() {
  StcConfig Result;
  auto TryName = [&](StringRef Name, TemplatePreset &ParsedOut) {
    TemplatePreset Parsed;
    if (!Name.empty() && parsePresetName(Name.trim(), Parsed)) {
      Result.Name = Name.trim().str();
      ParsedOut = Parsed;
      return true;
    }
    return false;
  };

  bool ProfileChosen = false;
  TemplatePreset ParsedProfile = TemplatePreset::None;
  if (auto Value = configValue("profile")) {
    if (TryName(*Value, ParsedProfile)) {
      Result.Preset = ParsedProfile;
      ProfileChosen = true;
    }
  }

  if (auto Value = configValue("stc_templates"))
    addNameList(*Value, Result.Templates);

  if (auto Value = configValue("stc_auto")) {
    if (!parseBoolName(*Value, Result.Auto))
      Result.Auto = false;
  }

  if ((!Result.Templates.empty() || Result.Auto) && !ProfileChosen) {
    Result.Preset = TemplatePreset::StcStatic;
    Result.Name = "stc";
  }
  return Result;
}

bool skipsStcGuard(const TemplateDef &Template, StringRef Arg) {
  return isStcPreset(Template) && isStcBuiltinType(Arg);
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
  // Implementation-source path -> instantiation markers used by this file.
  llvm::StringMap<llvm::StringSet<>> ImplWanted;
};

/// Shared state, surviving across AST builds and listener instances.
struct SharedState {
  TemplateRegistry Registry;
  TypeIndex Types;
  Instantiator Inst;
  DispatchConfig Dispatch;

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
  llvm::StringSet<> GeneratedImplSources;

  std::mutex UsageMutex;
  std::string Root;
  std::string ProfileName;
  llvm::StringMap<UsageFile> UsageCache;
  llvm::StringMap<llvm::StringSet<>> Wanted;
  llvm::StringMap<llvm::StringSet<>> WantedImpl;
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
    loadConfig(Root);
    StcConfig Config = resolvePreset();
    ProfileName = Config.Name;
    Dispatch = resolveDispatch();
    setDispatchTruncate(Dispatch.Truncate);
    Registry.scanPreferTemplateDirs(Root);
    SmallVector<StringRef, 4> Names;
    for (const std::string &Name : Config.Templates)
      Names.push_back(Name);
    Registry.applyPreset(Config.Preset, Names, Config.Auto);
    setDispatchAffix(Dispatch.Truncate ? "" : Dispatch.Affix,
                     Dispatch.Style == DispatchStyle::Prefix);
    setSourceScanDispatchTruncate(Dispatch.Truncate);
    Registry.applyDispatch(Dispatch);
    if (Dispatch.Enabled) {
      std::string Msg = "dispatch=1 backend=";
      Msg += Dispatch.Backend == DispatchBackend::Builtin ? "builtin"
                                                          : "generic";
      Msg += " style=";
      Msg += Dispatch.Style == DispatchStyle::Prefix ? "prefix" : "postfix";
      Msg += " affix=" + std::string(Dispatch.Truncate ? "none" : Dispatch.Affix);
      if (Dispatch.Truncate)
        Msg += " truncate=1";
      log(Msg);
    }
    if (!ProfileName.empty())
      log("using profile " + ProfileName);
    if (!Config.Templates.empty()) {
      std::string NameList;
      for (const std::string &Name : Config.Templates) {
        if (!NameList.empty())
          NameList += ',';
        NameList += Name;
      }
      log("using stc templates " + NameList);
    }
    if (Config.Auto)
      log("using stc auto=1");
    // Type guards are indexed lazily from the types actually used in the open
    // buffer. Scanning every unrelated header here would delay first parse.
    for (const auto &Def : Registry.templates()) {
      GeneratedHeaders.insert(Def->GeneratedHeader);
      if (!Def->GeneratedImplSource.empty())
        GeneratedImplSources.insert(Def->GeneratedImplSource);
    }

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
      for (size_t I = 0; I < Usage.Args.size(); ++I) {
        if (!isTypeArgument(*Usage.Template, I))
          continue;
        const std::string &Arg = Usage.Args[I];
        if (skipsStcGuard(*Usage.Template, Arg))
          continue;
        if (!needsIncludeGuard(Arg) || isGeneratedType(Arg) ||
            isTemplateConstructorArgument(Arg, Registry))
          continue;
        StringRef Name = lastIdentifier(Arg);
        if (!Name.empty())
          Added |= TypeNamesRequested.insert(Name.str()).second;
      }
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
                     llvm::StringMap<llvm::StringSet<>> &Out,
                     llvm::StringMap<llvm::StringSet<>> *ImplOut = nullptr)
      const {
    SmallVector<FoundUsage, 32> Usages;
    SmallVector<FoundMethodUsage, 8> MethodUsages;
    scanUsages(Text, Registry, Usages, &MethodUsages);
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
        Out[OutputHeader].insert(Marker);
        if (ImplOut && Usage.Template->HasTmplHeaderGuard &&
            !Usage.Template->GeneratedImplSource.empty())
          (*ImplOut)[Usage.Template->GeneratedImplSource].insert(
              std::move(Marker));
      }
    }

    for (const auto &OwnedMethod : Registry.methods()) {
      const DispatchMethodDef &Method = *OwnedMethod;
      for (const FoundMethodUsage &Found : MethodUsages) {
        if (Found.Method != &Method)
          continue;
        for (const FoundUsage &Usage : Usages) {
          if (Usage.IsNested || Usage.Template != Method.Owner)
            continue;
          SmallVector<std::string, 4> FullArgs;
          for (const std::string &Arg : Usage.Args)
            FullArgs.push_back(Arg);
          for (const std::string &Arg : Found.Args)
            FullArgs.push_back(Arg);
          std::string Marker = instantiationMarker(
              Method.Impl.Name, joinExplicitArgs(FullArgs));
          Out[Method.Impl.GeneratedHeader].insert(std::move(Marker));
        }
      }
    }
    (void)Path;
  }

  // Requires UsageMutex.
  void rebuildWanted() {
    Wanted.clear();
    WantedImpl.clear();
    for (const auto &File : UsageCache) {
      for (const auto &Entry : File.second.Wanted)
        for (const auto &Marker : Entry.second)
          Wanted[Entry.getKey()].insert(Marker.getKey());
      for (const auto &Entry : File.second.ImplWanted)
        for (const auto &Marker : Entry.second)
          WantedImpl[Entry.getKey()].insert(Marker.getKey());
    }
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
    collectUsages(Text, Path, Entry.Wanted, &Entry.ImplWanted);
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
      if (GeneratedHeaders.contains(Path) ||
          GeneratedImplSources.contains(Path))
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
    if (GeneratedImplSources.contains(makeAbsolute(Path)))
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
    writeImplMakefile();
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
    llvm::StringMap<llvm::StringSet<>> ImplSnapshot;
    {
      std::lock_guard<std::mutex> Lock(UsageMutex);
      if (!PruningReady)
        return;
      Snapshot = Wanted;
      ImplSnapshot = WantedImpl;
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

    for (const auto &Entry : GeneratedImplSources) {
      StringRef Path = Entry.getKey();
      const TemplateDef *Owner = nullptr;
      for (const TemplateDef *Template : Templates) {
        if (Template->GeneratedImplSource == Path) {
          Owner = Template;
          break;
        }
      }
      if (!Owner)
        continue;
      auto KeepIt = ImplSnapshot.find(Path);
      const llvm::StringSet<> &Keep =
          KeepIt == ImplSnapshot.end() ? Empty : KeepIt->second;
      Removed.clear();
      Error.clear();
      SmallVector<const TemplateDef *, 1> Single{Owner};
      if (!Inst.prune(Single, Path, Keep, Removed, Error)) {
        if (!Error.empty())
          log(std::string("prune failed: ") + Error);
        continue;
      }
      for (const std::string &Marker : Removed)
        log("removed " + Marker + " from " + Path.str());
    }
  }

  void writeImplMakefile() {
    if (GeneratedImplSources.empty())
      return;

    llvm::StringMap<llvm::StringSet<>> Snapshot;
    {
      std::lock_guard<std::mutex> Lock(UsageMutex);
      Snapshot = WantedImpl;
    }

    std::vector<std::string> Paths;
    for (const auto &Entry : Snapshot)
      if (!Entry.second.empty())
        Paths.push_back(Entry.getKey().str());
    std::sort(Paths.begin(), Paths.end());

    SmallString<256> Dir(Root);
    sys::path::append(Dir, "gen");
    sys::path::append(Dir, "impl");
    if (!sys::fs::is_directory(Dir)) {
      std::error_code EC = sys::fs::create_directories(Dir);
      if (EC) {
        log(std::string("failed creating ") + std::string(Dir.str()) + ": " +
            EC.message());
        return;
      }
    }

    SmallString<256> Path(Dir);
    sys::path::append(Path, "c-autotmpl.mk");

    std::string Content = "# Generated by c-autotmpl. Do not edit.\n";
    for (const std::string &Source : Paths) {
      Content += "C_AUTOTMPL_IMPL_SRCS += ";
      Content += computeRelativePath(Root, Source);
      Content += '\n';
    }

    std::string Old;
    if (auto Buffer = MemoryBuffer::getFile(Path, /*IsText=*/true))
      Old = (*Buffer)->getBuffer().str();
    if (Old == Content)
      return;

    std::error_code EC;
    raw_fd_ostream OS(Path, EC, sys::fs::CD_CreateAlways);
    if (EC) {
      log(std::string("failed writing ") + std::string(Path.str()) + ": " +
          EC.message());
      return;
    }
    OS << Content;
    OS.close();
    if (OS.has_error()) {
      log(std::string("failed writing ") + std::string(Path.str()));
      return;
    }
    log(std::string("generated makefile fragment ") + std::string(Path.str()));
  }

  void generateFromText(StringRef Text, clang::CompilerInstance *CI) {
    SmallVector<FoundUsage, 32> Usages;
    SmallVector<FoundMethodUsage, 8> MethodUsages;
    scanUsages(Text, Registry, Usages, &MethodUsages);
    generateUsages(Usages, CI, MethodUsages);
  }

  /// Groups each top-level usage with the nested usages emitted immediately
  /// before it. The whole chain is written to one generated header so mutually
  /// dependent types cannot be separated by include order.
  void generateUsages(ArrayRef<FoundUsage> Usages, clang::CompilerInstance *CI,
                      ArrayRef<FoundMethodUsage> MethodUsages = {}) {
    generateImplUsages(Usages, CI);
    llvm::StringMap<llvm::StringSet<>> AdapterOps;
    for (const FoundUsage &Usage : Usages)
      if (!Usage.AdapterOp.empty())
        AdapterOps[Usage.Template->Name].insert(Usage.AdapterOp);
    for (size_t I = 0; I < Usages.size(); ++I) {
      if (Usages[I].IsNested)
        continue;
      size_t Begin = I;
      while (Begin > 0 && Usages[Begin - 1].IsNested)
        --Begin;
      SmallVector<const FoundUsage *, 8> Chain;
      for (size_t J = Begin; J <= I; ++J)
        Chain.push_back(&Usages[J]);
      generateChain(Chain, CI, nullptr, AdapterOps);
    }
    generateDispatch(Usages, AdapterOps, MethodUsages, CI);
  }

  struct DispatchGroup {
    const TemplateDef *Template;
    std::string Output;
    llvm::SmallVector<DispatchCase, 2> Cases;
  };

  static DispatchGroup *findDispatchGroup(
      llvm::SmallVectorImpl<DispatchGroup> &Groups, const TemplateDef *Template,
      StringRef Output) {
    for (DispatchGroup &Group : Groups)
      if (Group.Template == Template && Group.Output == Output)
        return &Group;
    return nullptr;
  }

  void addDispatchCase(llvm::SmallVectorImpl<DispatchGroup> &Groups,
                       StringRef Output, const FoundUsage &Usage) {
    const TemplateDef *Template = Usage.Template;
    if (!Template || Template->Kind != TemplateKind::Type)
      return;
    DispatchGroup *Group = findDispatchGroup(Groups, Template, Output);
    if (!Group) {
      Groups.push_back({Template, Output.str(), {}});
      Group = &Groups.back();
    }
    for (const DispatchCase &Case : Group->Cases)
      if (Case.Type == Usage.TypeSpelling)
        return;
    Group->Cases.push_back({Usage.TypeSpelling,
                            dispatchContainerName(*Template, Usage.Args),
                            std::vector<std::string>(Usage.Args.begin(),
                                                     Usage.Args.end())});
  }

  /// Rewrites every dispatch section from scratch so added specializations,
  /// removed specializations, and env changes all stay in sync with the
  /// current source scan.
  void generateDispatch(
      ArrayRef<FoundUsage> Usages,
      const llvm::StringMap<llvm::StringSet<>> &AdapterOps = {},
      ArrayRef<FoundMethodUsage> MethodUsages = {},
      clang::CompilerInstance *CI = nullptr) {
    llvm::SmallVector<DispatchGroup, 4> Groups;
    for (const auto &Owned : Registry.templates()) {
      const TemplateDef *Template = Owned.get();
      if (Template->Kind != TemplateKind::Type)
        continue;
      // CLI-generated datatype headers already contain dispatch selectors
      // derived from their variant list. Keep them in the group so annotated
      // curried methods can be added, but never rewrite that base section.
      if (Template->HasDatatypeDispatch)
        log("keeping CLI dispatch for " + Template->Name);
      Groups.push_back({Template, Template->GeneratedHeader, {}});
    }
    for (const FoundUsage &Usage : Usages)
      addDispatchCase(Groups, Usage.Template->GeneratedHeader, Usage);
    for (size_t I = 0; I < Usages.size(); ++I) {
      if (Usages[I].IsNested)
        continue;
      size_t Begin = I;
      while (Begin > 0 && Usages[Begin - 1].IsNested)
        --Begin;
      StringRef Output = Usages[I].Template->GeneratedHeader;
      for (size_t J = Begin; J <= I; ++J)
        addDispatchCase(Groups, Output, Usages[J]);
    }

    // Curried methods cross every owner specialization with every explicit
    // type tuple seen at a `map(float)(...)` call. The implementation template
    // is synthesized like any other function specialization, then the method
    // section is emitted into the owner's generated header.
    for (const auto &OwnedMethod : Registry.methods()) {
      const DispatchMethodDef &Method = *OwnedMethod;
      if (!Method.Owner || !Method.Owner->DispatchEnabled)
        continue;
      DispatchGroup *Group = findDispatchGroup(
          Groups, Method.Owner, Method.Owner->GeneratedHeader);
      if (!Group)
        continue;

      SmallVector<DispatchMethodUse, 4> Uses;
      for (const FoundMethodUsage &Found : MethodUsages) {
        if (Found.Method != &Method)
          continue;
        bool Duplicate = false;
        for (const DispatchMethodUse &Use : Uses)
          if (Use.Key == Found.Key)
            Duplicate = true;
        if (Duplicate)
          continue;
        DispatchMethodUse Use;
        Use.Key = Found.Key;
        for (const std::string &Arg : Found.Args)
          Use.Args.push_back(Arg);
        Uses.push_back(std::move(Use));
      }

      for (const DispatchMethodUse &Use : Uses) {
        for (const DispatchCase &Case : Group->Cases) {
          FoundUsage ImplUsage;
          ImplUsage.Template = &Method.Impl;
          SmallVector<std::string, 4> FullArgs;
          for (const std::string &Arg : Case.Args)
            FullArgs.push_back(Arg);
          for (const std::string &Arg : Use.Args)
            FullArgs.push_back(Arg);
          ImplUsage.TypeSpelling = joinExplicitArgs(FullArgs);
          for (const std::string &Arg : FullArgs)
            ImplUsage.Args.push_back(Arg);
          generateInto(ImplUsage, Method.Impl.GeneratedHeader, CI, {}, {});
        }
      }

      std::string Error;
      Inst.ensureDispatchMethod(Method, Group->Cases, Uses, Dispatch,
                                Group->Output, Error);
      if (!Error.empty())
        log(std::string("failed explicit method dispatch: ") + Error);
    }

    for (const DispatchGroup &Group : Groups) {
      if (Group.Template->HasDatatypeDispatch)
        continue;
      TemplateDef DispatchTemplate = *Group.Template;
      SmallVector<std::string, 4> OwnedOps;
      llvm::StringSet<> SeenOps;
      for (const auto &Entry : Group.Template->DispatchOps) {
        if (SeenOps.insert(Entry.getKey()).second)
          OwnedOps.push_back(Entry.getKey().str());
      }
      auto It = AdapterOps.find(Group.Template->Name);
      if (It != AdapterOps.end())
        for (const auto &Entry : It->second) {
          std::string PublicOp =
              (Group.Template->Name + "_" + Entry.getKey()).str();
          DispatchTargetInfo Info;
          Info.Suffix = Entry.getKey().str();
          DispatchTemplate.DispatchTargets[PublicOp] = Info;
          if (SeenOps.insert(PublicOp).second)
            OwnedOps.push_back(PublicOp);
        }
      SmallVector<StringRef, 4> Ops;
      for (const std::string &Op : OwnedOps)
        Ops.push_back(Op);
      std::string Error;
      Inst.ensureDispatch(DispatchTemplate, Group.Cases, Ops, Dispatch,
                          Group.Output, Error);
      if (!Error.empty())
        log(std::string("failed dispatch: ") + Error);
    }
  }

  bool generateChain(ArrayRef<const FoundUsage *> Chain,
                     clang::CompilerInstance *CI,
                     std::string *GenPathOut = nullptr,
                     const llvm::StringMap<llvm::StringSet<>> &AdapterOps = {}) {
    if (Chain.empty())
      return false;
    const std::string OutputHeader = Chain.back()->Template->GeneratedHeader;
    if (GenPathOut)
      *GenPathOut = OutputHeader;
    bool Added = false;
    for (const FoundUsage *Usage : Chain) {
      Added |= generateInto(*Usage, OutputHeader, CI, Chain, AdapterOps);
      // Keep the dependency's own generated header up to date as well. The
      // presence macro makes the two copies harmless, and it guarantees the
      // header exists for sources that include it before this one.
      if (Usage != Chain.back() &&
          Usage->Template->GeneratedHeader != OutputHeader)
        Added |= generateInto(*Usage, Usage->Template->GeneratedHeader, CI, {},
                              AdapterOps);
    }
    return Added;
  }

  /// Writes one usage block into OutputHeader, which may belong to an outer
  /// template when this is a nested dependency.
  bool generateInto(const FoundUsage &Usage, StringRef OutputHeader,
                    clang::CompilerInstance *CI,
                    ArrayRef<const FoundUsage *> Chain,
                    const llvm::StringMap<llvm::StringSet<>> &AdapterOps) {
    SmallVector<StringRef, 4> Ops;
    auto OpIt = AdapterOps.find(Usage.Template->Name);
    if (OpIt != AdapterOps.end())
      for (const auto &Entry : OpIt->second)
        Ops.push_back(Entry.getKey());
    std::string AdapterError;
    bool AdaptersAdded =
        Inst.ensureAdapters(*Usage.Template, Ops, OutputHeader, AdapterError);
    if (!AdapterError.empty())
      log(std::string("failed adapters: ") + AdapterError);
    if (Inst.has(*Usage.Template, Usage.TypeSpelling, OutputHeader))
      return AdaptersAdded;

    SmallVector<DependencyGuard, 2> Guards;
    dependencyGuards(Usage, OutputHeader, Chain, Guards);

    SmallVector<std::string, 4> TypeGuards;
    llvm::StringSet<> SeenTypeGuards;
    bool MissingGuard = false;
    for (size_t I = 0; I < Usage.Args.size(); ++I)
      if (isTypeArgument(*Usage.Template, I) &&
          !skipsStcGuard(*Usage.Template, Usage.Args[I]))
        MissingGuard |=
            !collectTypeGuard(Usage.Args[I], CI, 0, SeenTypeGuards, TypeGuards);
    if (MissingGuard) {
      warnNoGuardOnce(Usage.Template->Name, Usage.TypeSpelling);
      return false;
    }
    for (const std::string &Guard : TypeGuards) {
      bool Seen = false;
      for (const DependencyGuard &Dep : Guards)
        if (Dep.Guard == Guard)
          Seen = true;
      if (!Seen)
        Guards.push_back({Guard});
    }

    std::string GenPath, Error;
    InstantiationResult Result = Inst.instantiate(
        *Usage.Template, Usage.TypeSpelling, "", GenPath, Error,
        /*DryRun=*/false, Guards, OutputHeader, Ops);
    if (Result == InstantiationResult::Error) {
      log(std::string("failed: ") + Error);
      return false;
    }
    if (Result == InstantiationResult::Added) {
      log(std::string("generated ") + Usage.Template->Name + "(" +
          Usage.TypeSpelling + ") in " + GenPath);
      return true;
    }
    return AdaptersAdded;
  }

  bool isGeneratedType(StringRef Spelling) const {
    StringRef Type = stripTypeQualifiers(Spelling);
    StringRef Arg;
    return Registry.matchMangled(Type, Arg) != nullptr;
  }

  /// Adds the include guard needed by one type argument, if any. Generated
  /// template arguments recurse into their own arguments, e.g. Box_Foo needs
  /// FOO_H. Returns false when a user-defined type has no known guard.
  bool collectTypeGuard(StringRef Spelling, clang::CompilerInstance *CI,
                        unsigned Depth, llvm::StringSet<> &Seen,
                        SmallVectorImpl<std::string> &Guards) {
    if (Depth > 16 || !needsIncludeGuard(Spelling) ||
        isTemplateConstructorArgument(Spelling, Registry))
      return true;

    StringRef Type = stripTypeQualifiers(Spelling);
    StringRef Arg;
    if (Registry.matchMangled(Type, Arg))
      return collectTypeGuard(Arg, CI, Depth + 1, Seen, Guards);

    std::string Guard;
    {
      std::lock_guard<std::mutex> Lock(TypesMutex);
      Guard = Types.guardForType(Type);
    }
    if (Guard.empty() && CI) {
      for (const std::string &Header : definingHeaderPaths(*CI, Type)) {
        Guard = guardForPath(Header);
        if (!Guard.empty())
          break;
      }
    }
    if (Guard.empty())
      return false;
    if (Seen.insert(Guard).second)
      Guards.push_back(std::move(Guard));
    return true;
  }

  /// True when the generated block exists and all of its conditional guards
  /// are satisfied in the current parse. Used to tell a stale-parse diagnostic
  /// from a real missing include or unsatisfied type guard.
  bool blockIsUsable(const FoundUsage &Usage, StringRef OutputHeader,
                     ArrayRef<const FoundUsage *> Chain,
                     clang::CompilerInstance &CI) {
    if (!Inst.has(*Usage.Template, Usage.TypeSpelling, OutputHeader))
      return false;

    SmallVector<DependencyGuard, 2> Deps;
    dependencyGuards(Usage, OutputHeader, Chain, Deps);
    StringSet<> Seen;
    SmallVector<std::string, 4> Guards;
    for (const DependencyGuard &Dep : Deps)
      if (Seen.insert(Dep.Guard).second)
        Guards.push_back(Dep.Guard);

    bool MissingGuard = false;
    for (size_t I = 0; I < Usage.Args.size(); ++I)
      if (isTypeArgument(*Usage.Template, I) &&
          !skipsStcGuard(*Usage.Template, Usage.Args[I]))
        MissingGuard |= !collectTypeGuard(Usage.Args[I], &CI, 0, Seen, Guards);
    if (MissingGuard)
      return false;

    for (const std::string &Guard : Guards)
      if (!CI.getPreprocessor().isMacroDefined(Guard))
        return false;
    return true;
  }

  /// Like collectTypeGuard, but also records the header that declared the
  /// type. Implementation TUs include those headers before re-including the
  /// template so trait declarations are visible to the definition branch.
  bool collectImplArg(StringRef Spelling, clang::CompilerInstance *CI,
                      unsigned Depth, llvm::StringSet<> &SeenGuards,
                      llvm::StringSet<> &SeenHeaders,
                      SmallVectorImpl<std::string> &Guards,
                      SmallVectorImpl<std::string> &Headers) {
    if (Depth > 16 || !needsIncludeGuard(Spelling) ||
        isTemplateConstructorArgument(Spelling, Registry))
      return true;

    StringRef Type = stripTypeQualifiers(Spelling);
    StringRef Arg;
    if (Registry.matchMangled(Type, Arg))
      return collectImplArg(Arg, CI, Depth + 1, SeenGuards, SeenHeaders,
                            Guards, Headers);

    std::string Guard;
    std::string Header;
    {
      std::lock_guard<std::mutex> Lock(TypesMutex);
      Guard = Types.guardForType(Type);
      Header = Types.headerForType(Type);
    }
    if (Guard.empty() && CI) {
      for (const std::string &Candidate : definingHeaderPaths(*CI, Type)) {
        std::string Found = guardForPath(Candidate);
        if (!Found.empty()) {
          Guard = std::move(Found);
          Header = Candidate;
          break;
        }
      }
    }
    if (Guard.empty())
      return false;
    if (SeenGuards.insert(Guard).second)
      Guards.push_back(std::move(Guard));
    if (!Header.empty() && SeenHeaders.insert(Header).second)
      Headers.push_back(std::move(Header));
    return true;
  }

  void generateImplUsages(ArrayRef<FoundUsage> Usages,
                          clang::CompilerInstance *CI) {
    for (const FoundUsage &Usage : Usages) {
      const TemplateDef &Template = *Usage.Template;
      if (!Template.HasTmplHeaderGuard ||
          !GeneratedImplSources.contains(Template.GeneratedImplSource))
        continue;
      if (Inst.has(Template, Usage.TypeSpelling, Template.GeneratedImplSource))
        continue;

      llvm::StringSet<> SeenGuards;
      llvm::StringSet<> SeenHeaders;
      SmallVector<std::string, 4> Guards;
      SmallVector<std::string, 4> Headers;
      bool MissingGuard = false;
      for (size_t I = 0; I < Usage.Args.size(); ++I)
        if (isTypeArgument(Template, I) &&
            !skipsStcGuard(Template, Usage.Args[I]))
          MissingGuard |= !collectImplArg(Usage.Args[I], CI, 0, SeenGuards,
                                          SeenHeaders, Guards, Headers);
      if (MissingGuard) {
        warnNoGuardOnce(Template.Name, Usage.TypeSpelling);
        continue;
      }

      std::string ImplDir =
          sys::path::parent_path(Template.GeneratedImplSource).str();
      llvm::StringSet<> SeenIncludes;
      SmallVector<std::string, 4> IncludeLines;
      for (const std::string &Header : Headers) {
        std::string Include = "#include \"" +
                              computeRelativePath(ImplDir, Header) + "\"";
        if (SeenIncludes.insert(Include).second)
          IncludeLines.push_back(std::move(Include));
      }

      std::string GenPath;
      std::string Error;
      InstantiationResult Result =
          Inst.instantiateImpl(Template, Usage.TypeSpelling, Usage.Args, Guards,
                               IncludeLines, GenPath, Error);
      if (Result == InstantiationResult::Error) {
        log(std::string("failed: ") + Error);
        continue;
      }
      if (Result == InstantiationResult::Added)
        log("generated implementation " + Template.Name + "(" +
            Usage.TypeSpelling + ") in " + GenPath);
    }
  }

  void dependencyGuards(const FoundUsage &Usage, StringRef OutputHeader,
                        ArrayRef<const FoundUsage *> Chain,
                        SmallVectorImpl<DependencyGuard> &Guards) {
    llvm::StringSet<> Seen;
    for (const UsageDep &Dep : Usage.DependsOn) {
      // Dependencies emitted earlier in this chain are already present in the
      // output header, so they need no cross-header guard.
      if (Dep.Template->GeneratedHeader == OutputHeader)
        continue;
      bool InChain = false;
      for (const FoundUsage *Member : Chain)
        if (Member->Template == Dep.Template &&
            Member->TypeSpelling == Dep.TypeSpelling) {
          InChain = true;
          break;
        }
      if (InChain)
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

struct DispatchNavTarget {
  const TemplateDef *Template = nullptr;
  std::string Op;
};

static bool navIdentStart(char C) {
  return C == '_' || (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z');
}

static bool navIdentChar(char C) {
  return navIdentStart(C) || (C >= '0' && C <= '9');
}

static bool navIdent(StringRef Text) {
  if (Text.empty() || !navIdentStart(Text.front()))
    return false;
  for (char C : Text)
    if (!navIdentChar(C))
      return false;
  return true;
}

static bool dispatchAffixIsIdent(const DispatchConfig &Config) {
  return !Config.Affix.empty() && navIdentStart(Config.Affix.front());
}

/// Recognizes `$vec_int_push(...)` or `vec_int_push$(...)` at the cursor and
/// maps the function spelling back to its template and operation.
static DispatchNavTarget dispatchTargetAt(StringRef Text, size_t Offset,
                                          const SharedState &State) {
  DispatchNavTarget Result;
  if (!State.Dispatch.Enabled || Offset > Text.size())
    return Result;

  size_t Start = Offset;
  while (Start > 0 && navIdentChar(Text[Start - 1]))
    --Start;
  size_t End = Offset;
  while (End < Text.size() && navIdentChar(Text[End]))
    ++End;
  if (Start == End)
    return Result;

  StringRef Ident = Text.slice(Start, End);
  StringRef Affix(State.Dispatch.Affix);
  bool AffixIsIdent = dispatchAffixIsIdent(State.Dispatch);
  bool IsDispatch = State.Dispatch.Truncate;
  if (!State.Dispatch.Truncate) {
    if (State.Dispatch.Style == DispatchStyle::Prefix) {
      if (AffixIsIdent)
        IsDispatch = Affix.size() < Ident.size() && Ident.starts_with(Affix);
      else
        IsDispatch = Start >= Affix.size() &&
                     Text.substr(Start - Affix.size(), Affix.size()) == Affix;
    } else {
      if (AffixIsIdent)
        IsDispatch = Affix.size() < Ident.size() && Ident.ends_with(Affix);
      else
        IsDispatch = Text.substr(End).starts_with(Affix);
    }
  }
  if (!IsDispatch)
    return Result;

  if (AffixIsIdent && !State.Dispatch.Truncate)
    Ident = State.Dispatch.Style == DispatchStyle::Prefix
                ? Ident.drop_front(Affix.size())
                : Ident.drop_back(Affix.size());

  if (!navIdent(Ident))
    return Result;
  for (const auto &Owned : State.Registry.templates()) {
    const TemplateDef *Def = Owned.get();
    if (!Def || !Def->DispatchEnabled || Def->Kind != TemplateKind::Type ||
        Def->Name.empty() || !Def->DispatchOps.contains(Ident))
      continue;
    Result.Template = Def;
    Result.Op = Ident.str();
    break;
  }
  return Result;
}

static std::string escapedSnippetAffix(StringRef Affix) {
  std::string Out;
  for (char C : Affix) {
    if (C == '$')
      Out.push_back('\\');
    Out.push_back(C);
  }
  return Out;
}

/// clangd emits snippet completions with the raw macro name. Clients treat a
/// leading `$` as snippet-variable syntax and drop it when accepting the item.
/// Escape only the configured affix so clangd's `${1:...}` placeholders keep
/// working.
static std::string escapeDispatchAffixInSnippet(StringRef Text,
                                                const DispatchConfig &Config) {
  StringRef Affix(Config.Affix);
  if (Affix.empty() || Affix.find('$') == StringRef::npos)
    return Text.str();
  std::string Escaped = escapedSnippetAffix(Affix);
  if (Config.Style == DispatchStyle::Prefix) {
    if (!Text.starts_with(Affix))
      return Text.str();
    return Escaped + Text.drop_front(Affix.size()).str();
  }
  size_t NameEnd = Text.find('(');
  if (NameEnd == StringRef::npos)
    NameEnd = Text.size();
  StringRef Name = Text.take_front(NameEnd);
  if (!Name.ends_with(Affix))
    return Text.str();
  return Name.drop_back(Affix.size()).str() + Escaped +
         Text.substr(NameEnd).str();
}

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
    State->generateDispatch(Usages);
    State->generateImplUsages(Usages, CI);
    State->writeImplMakefile();
    bool IncludePresent = sourceIncludesGeneratedHeader(SM, GenPath);
    addIncludeFix(SM, Outer.Template->Name, GenPath, D, IncludePresent);

    // A generated header can be written after clangd has already started this
    // parse, leaving the AST stale for one pass. If the source already includes
    // the header and the new block is active under the current preprocessor,
    // the reparse will resolve this diagnostic. Drop it so the editor never
    // flashes red. Without the include, keep the diagnostic and its fix.
    if (IncludePresent && !GenPath.empty() &&
        State->blockIsUsable(Outer,
                             Chain.back()->Template->GeneratedHeader, Chain,
                             *CI)) {
      log("suppressed transient diagnostic for " + Outer.Template->Name + "(" +
          Outer.TypeSpelling + ")");
      D.Severity = DiagnosticsEngine::Ignored;
    }
  }

private:
  bool sourceIncludesGeneratedHeader(const clang::SourceManager &SM,
                                     StringRef GenPath) const {
    if (MainFile.empty() || GenPath.empty())
      return false;
    std::string Rel =
        computeRelativePath(sys::path::parent_path(MainFile), GenPath);
    // Sources in a subdirectory normally include generated headers spelling
    // the path from the project root, e.g. "gen/box.h" rather than
    // "../gen/box.h". Accept that spelling too, or we add a duplicate include
    // at offset 0 and reorder the user's include block on every diagnostic.
    std::string FromRoot = computeRelativePath(State->Root, GenPath);
    bool Invalid = false;
    StringRef Buffer = SM.getBufferData(SM.getMainFileID(), &Invalid);
    return !Invalid && (Buffer.contains(Rel) || Buffer.contains(FromRoot) ||
                        Buffer.contains(GenPath));
  }

  void addIncludeFix(const clang::SourceManager &SM, StringRef TemplateName,
                     StringRef GenPath, Diag &D, bool IncludePresent) {
    if (MainFile.empty() || AddedInclude)
      return;
    if (IncludePresent) {
      AddedInclude = true;
      return;
    }

    std::string Rel =
        computeRelativePath(sys::path::parent_path(MainFile), GenPath);
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
    wrapMethod(Bind, "textDocument/definition");
    wrapMethod(Bind, "textDocument/completion");
  }

private:
  using Notification = llvm::unique_function<void(llvm::json::Value)>;
  using Method = llvm::unique_function<void(llvm::json::Value,
                                            Callback<llvm::json::Value>)>;

  void wrapMethod(LSPBinder &Bind, llvm::StringLiteral MethodName) {
    auto It = Bind.Raw.MethodHandlers.find(MethodName);
    if (It == Bind.Raw.MethodHandlers.end())
      return;
    Method Original = std::move(It->second);
    Bind.Raw.MethodHandlers[MethodName] =
        [this, MethodName, Original = std::move(Original)](
            llvm::json::Value Raw, Callback<llvm::json::Value> Reply) mutable {
          if (MethodName == "textDocument/definition" &&
              goToTemplateFunction(Raw, Reply))
            return;
          if (MethodName == "textDocument/completion" && Original) {
            Original(std::move(Raw),
                     [this, Reply = std::move(Reply)](
                         llvm::Expected<llvm::json::Value> Result) mutable {
                       if (Result)
                         patchCompletionSnippets(*Result);
                       Reply(std::move(Result));
                     });
            return;
          }
          if (Original)
            Original(std::move(Raw), std::move(Reply));
          else
            Reply(nullptr);
        };
  }

  /// Native clangd go-to-def lands on the generated `#define`. For a c-autotmpl
  /// dispatch call we answer with the template header operation instead.
  bool goToTemplateFunction(llvm::json::Value &Raw,
                            Callback<llvm::json::Value> &Reply) {
    TextDocumentPositionParams Params;
    llvm::json::Path::Root Root;
    if (!fromJSON(Raw, Params, Root))
      return false;

    StringRef MainFile = Params.textDocument.uri.file();
    if (MainFile.empty())
      return false;
    State->ensureScanned(MainFile);

    std::string Text = State->openText(MainFile);
    if (Text.empty()) {
      auto Buffer = MemoryBuffer::getFile(MainFile, /*IsText=*/true);
      if (!Buffer)
        return false;
      Text = (*Buffer)->getBuffer().str();
    }
    auto Offset = positionToOffset(Text, Params.position);
    if (!Offset) {
      consumeError(Offset.takeError());
      return false;
    }

    DispatchNavTarget Target =
        dispatchTargetAt(Text, *Offset, *State);
    if (!Target.Template)
      return false;

    auto HeaderBuffer =
        MemoryBuffer::getFile(Target.Template->Header, /*IsText=*/true);
    if (!HeaderBuffer)
      return false;
    StringRef HeaderText = (*HeaderBuffer)->getBuffer();
    unsigned HeaderOffset = 0;
    if (auto It = Target.Template->DispatchOpOffsets.find(Target.Op);
        It != Target.Template->DispatchOpOffsets.end())
      HeaderOffset = It->second;
    else if (size_t Found = HeaderText.find(Target.Op);
             Found != StringRef::npos)
      HeaderOffset = static_cast<unsigned>(Found);
    else if (size_t Found = HeaderText.find(Target.Template->Name);
             Found != StringRef::npos)
      HeaderOffset = static_cast<unsigned>(Found);
    if (HeaderOffset >= HeaderText.size())
      HeaderOffset = 0;

    Location Loc;
    Loc.uri = URIForFile::canonicalize(Target.Template->Header, MainFile);
    Loc.range.start = offsetToPosition(HeaderText, HeaderOffset);
    Loc.range.end = Loc.range.start;
    llvm::json::Array Result;
    Result.push_back(toJSON(Loc));
    Reply(std::move(Result));
    return true;
  }

  /// Keep dispatch affixes literal inside snippet completions. clangd's raw
  /// `$name` is consumed as a snippet variable by common LSP clients, which
  /// removes the `$` when the completion is accepted.
  void patchCompletionSnippets(llvm::json::Value &Result) {
    if (!State->Dispatch.Enabled)
      return;
    StringRef Affix(State->Dispatch.Affix);
    if (Affix.empty() || Affix.find('$') == StringRef::npos)
      return;

    llvm::json::Array *Items = nullptr;
    if (auto *Object = Result.getAsObject())
      Items = Object->getArray("items");
    else
      Items = Result.getAsArray();
    if (!Items)
      return;

    for (llvm::json::Value &Entry : *Items) {
      auto *Item = Entry.getAsObject();
      if (!Item)
        continue;
      auto Format = Item->getInteger("insertTextFormat");
      if (!Format || *Format != 2)
        continue;
      if (auto Insert = Item->getString("insertText")) {
        std::string Fixed =
            escapeDispatchAffixInSnippet(*Insert, State->Dispatch);
        if (Fixed != *Insert)
          (*Item)["insertText"] = std::move(Fixed);
      }
      if (auto *Edit = Item->getObject("textEdit")) {
        if (auto NewText = Edit->getString("newText")) {
          std::string Fixed =
              escapeDispatchAffixInSnippet(*NewText, State->Dispatch);
          if (Fixed != *NewText)
            (*Edit)["newText"] = std::move(Fixed);
        }
      }
    }
  }

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
