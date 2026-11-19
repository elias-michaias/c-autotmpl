#ifndef C_AUTOTMPL_TEMPLATE_REGISTRY_H
#define C_AUTOTMPL_TEMPLATE_REGISTRY_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <memory>
#include <string>
#include <vector>

namespace c_autotmpl {

/// A template header discovered from a decorator such as:
///
///   // -- template List(T)
///   // -- template type Box(T)
struct TemplateDef {
  std::string Name;                            // "List"
  llvm::SmallVector<std::string, 2> Params;    // {"T"}
  std::string Header;                          // absolute tmpl/list.h
  std::string GeneratedHeader;                 // absolute gen/list.h
  std::string IncludeFromGenDir;               // "../tmpl/list.h"
};

/// Owns template definitions and answers hot-path lookups with StringRefs.
/// Scanning happens once; lookups are simple hash/prefix comparisons.
class TemplateRegistry {
public:
  void scan(llvm::StringRef Root);

  /// Scans `Root/tmpl` or `Root/templates` first and stops if decorators are
  /// found there. Falls back to the full recursive scan for other layouts.
  void scanPreferTemplateDirs(llvm::StringRef Root);

  const TemplateDef *findByName(llvm::StringRef Name) const {
    auto It = ByName.find(Name);
    return It == ByName.end() ? nullptr : It->second;
  }

  /// Matches e.g. "List_int" to template "List" with type spelling "int".
  /// Returns nullptr if no known template is a prefix.
  const TemplateDef *matchMangled(llvm::StringRef Mangled,
                                  llvm::StringRef &TypeSpelling) const {
    for (const TemplateDef *Def : ByLength) {
      llvm::StringRef Prefix(Def->Name);
      if (Mangled.size() <= Prefix.size() + 1 || Mangled[Prefix.size()] != '_')
        continue;
      if (Mangled.starts_with(Prefix)) {
        TypeSpelling = Mangled.drop_front(Prefix.size() + 1);
        return Def;
      }
    }
    return nullptr;
  }

  /// Cheap prefilter: true when Text mentions any known template name. Avoids
  /// tokenizing unrelated headers during workspace usage scans.
  bool mayContainUsage(llvm::StringRef Text) const {
    for (const auto &Def : Owned)
      if (Text.find(Def->Name) != llvm::StringRef::npos)
        return true;
    return false;
  }

  bool empty() const { return Owned.empty(); }
  const std::vector<std::unique_ptr<TemplateDef>> &templates() const {
    return Owned;
  }
  llvm::StringRef root() const { return Root; }

private:
  void scanTree(llvm::StringRef Dir);
  void finishScan();

  std::string Root;
  // Stable addresses: scan creates owned entries, maps only hold pointers.
  std::vector<std::unique_ptr<TemplateDef>> Owned;
  llvm::StringMap<const TemplateDef *> ByName;
  // Longest names first, so "List_List" doesn't match "List".
  llvm::SmallVector<const TemplateDef *, 8> ByLength;
};

/// Returns the include guard macro near the top of Header, or an empty
/// StringRef-like std::string when the header has no recognisable guard.
std::string findIncludeGuard(llvm::StringRef Path);

/// Same check for an already-loaded buffer. Used by scanners that need the
/// contents anyway, to avoid a second file read.
std::string findIncludeGuardInBuffer(llvm::StringRef Content);

/// Returns the file guard of a generated header, falling back to the default
/// guard name derived from its path when the file does not exist yet.
std::string generatedHeaderFileGuard(llvm::StringRef GenPath);

/// Computes a relative path with '/' separators from FromDir to ToFile.
std::string computeRelativePath(llvm::StringRef FromDir, llvm::StringRef ToFile);

/// Makes Path absolute without touching the filesystem.
std::string makeAbsolute(llvm::StringRef Path);

} // namespace c_autotmpl

#endif // C_AUTOTMPL_TEMPLATE_REGISTRY_H
