#ifndef C_AUTOTMPL_INSTANTIATOR_H
#define C_AUTOTMPL_INSTANTIATOR_H

#include "TemplateRegistry.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace c_autotmpl {

enum class InstantiationResult { Added, Existing, Error };

/// Include guard of a generated dependency header. The whole instantiation
/// is wrapped in these guards, so it is emitted only when the dependency's
/// generated header has already been included by the source.
struct DependencyGuard {
  std::string Guard;
};

/// Caches generated-header contents and append-only instantiation markers.
/// Shared between AST worker threads; all mutations are serialized.
class Instantiator {
public:
  /// True when the generated header already contains this instantiation.
  /// Cheaper than computing a guard for an existing block. OutputHeader lets
  /// nested dependencies be checked in the outermost generated header.
  bool has(const TemplateDef &Template, llvm::StringRef TypeSpelling,
           llvm::StringRef OutputHeader = {});

  /// Removes generated blocks that are not in Keep. Returns true and fills
  /// Removed when the generated header changed.
  bool prune(const TemplateDef &Template, const llvm::StringSet<> &Keep,
             std::vector<std::string> &Removed, std::string &Error);

  /// Prunes every known template's blocks from OutputHeader. Nested chains are
  /// flattened into the outermost generated header, so a header can contain
  /// blocks belonging to more than one template.
  bool prune(llvm::ArrayRef<const TemplateDef *> Templates,
             llvm::StringRef OutputHeader, const llvm::StringSet<> &Keep,
             std::vector<std::string> &Removed, std::string &Error);

  InstantiationResult instantiate(
      const TemplateDef &Template, llvm::StringRef TypeSpelling,
      llvm::StringRef GuardMacro, std::string &GenPath, std::string &Error,
      bool DryRun = false,
      llvm::ArrayRef<DependencyGuard> ExtraGuards = {},
      llvm::StringRef OutputHeader = {});

  /// Writes one block into the template's grouped implementation source.
  /// IncludeLines are already relative to the implementation source directory.
  InstantiationResult instantiateImpl(
      const TemplateDef &Template, llvm::StringRef TypeSpelling,
      llvm::ArrayRef<std::string> Args,
      llvm::ArrayRef<std::string> GuardMacros,
      llvm::ArrayRef<std::string> IncludeLines, std::string &GenPath,
      std::string &Error);

private:
  struct FileState {
    std::string Content;
    llvm::StringSet<> Markers;
    uint64_t Stamp = 0;
  };

  FileState &getState(llvm::StringRef Path);
  void loadState(FileState &State, llvm::StringRef Path, uint64_t Stamp);

  std::mutex Mutex;
  llvm::StringMap<std::unique_ptr<FileState>> Files;
};

/// Marker used to identify an instantiation block. Exposed so scanners can
/// compare wanted blocks without regenerating their text.
std::string instantiationMarker(llvm::StringRef TemplateName,
                                llvm::StringRef TypeSpelling);

/// Builds the text of one instantiation block. Exposed for tests.
std::string makeInstantiationBlock(
    const TemplateDef &Template, llvm::StringRef TypeSpelling,
    llvm::StringRef GuardMacro,
    llvm::ArrayRef<DependencyGuard> ExtraGuards = {});

/// Presence macro used by generated implementation sources. It deliberately
/// uses a different prefix from public headers so the two block sets cannot
/// collide.
std::string implPresenceMacro(llvm::StringRef TemplateName,
                              llvm::StringRef TypeSpelling);

/// Builds one grouped implementation block. Args are the canonical individual
/// template arguments; GuardMacros are traditional include guards to wrap it.
std::string makeImplInstantiationBlock(
    const TemplateDef &Template, llvm::StringRef TypeSpelling,
    llvm::ArrayRef<std::string> Args,
    llvm::ArrayRef<std::string> GuardMacros);

} // namespace c_autotmpl

#endif // C_AUTOTMPL_INSTANTIATOR_H
