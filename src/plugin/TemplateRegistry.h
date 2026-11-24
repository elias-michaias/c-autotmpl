#ifndef C_AUTOTMPL_TEMPLATE_REGISTRY_H
#define C_AUTOTMPL_TEMPLATE_REGISTRY_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <memory>
#include <string>
#include <vector>

namespace c_autotmpl {

enum class TemplateKind { Type, Function };

/// Explicit argument roles used by opt-in presets. Type arguments keep the
/// ordinary guard and dependency rules; the other roles are passed through
/// without being treated as type names.
enum class ArgRole { Type, Name, Flags, Opaque, Key, Value };

/// Opt-in emitter policy. None is the original, unchanged behavior.
enum class TemplatePreset { None, StcStatic, StcShared };

struct ArgSpec {
  ArgRole Role = ArgRole::Type;
  std::string Macro; // Empty means "use the decorator parameter name".
};

/// A template header discovered from a decorator such as:
///
///   // -- template List(T)
///   // -- template type Box(T)
///   // -- template function select(First, Second)
struct TemplateDef {
  TemplateKind Kind = TemplateKind::Type;
  std::string Name;                            // "List"
  llvm::SmallVector<std::string, 2> Params;    // {"T"}
  std::string Header;                          // absolute tmpl/list.h
  std::string GeneratedHeader;                 // absolute gen/list.h
  std::string IncludeFromGenDir;               // "../tmpl/list.h"
  std::string GeneratedImplSource;             // absolute gen/impl/list.c
  std::string IncludeFromImplDir;              // "../../tmpl/list.h"
  bool HasTmplHeaderGuard = false;             // has `#ifndef TMPL_HEADER`
  TemplatePreset Preset = TemplatePreset::None;
  llvm::SmallVector<ArgSpec, 2> ArgSpecs;      // empty means legacy behavior
};

/// True for arguments that need include-guard and dependency handling.
bool isTypeArgument(const TemplateDef &Template, size_t Index);

/// Decorator parameter name at Index, defaulting to T for undeclared templates.
llvm::StringRef parameterName(const TemplateDef &Template, size_t Index);

/// Macro defined by the generated block for argument Index.
llvm::StringRef argumentMacroName(const TemplateDef &Template, size_t Index);

bool parsePresetName(llvm::StringRef Name, TemplatePreset &Out);

/// True when the template uses STC macro names and split mapping.
bool isStcPreset(const TemplateDef &Template);

/// New-style STC wrappers declare only type-like parameters, so names and
/// trait defaults are derived instead of passed at the call site.
bool usesDerivedStcNames(const TemplateDef &Template);

/// Conservative content-based check used by opt-in STC auto-detection.
bool looksLikeStcHeader(const TemplateDef &Template);

/// True for STC-owned types that need no c-autotmpl include guard.
bool isStcBuiltinType(llvm::StringRef Spelling);

/// Derived container type name, such as stc_vec_int.
std::string stcContainerName(const TemplateDef &Template,
                             llvm::ArrayRef<std::string> Args);

/// Derived trait flags expression, or empty when STC needs no i_traits.
std::string stcTraits(const TemplateDef &Template,
                      llvm::ArrayRef<std::string> Args);

/// Owns template definitions and answers hot-path lookups with StringRefs.
/// Scanning happens once; lookups are simple hash/prefix comparisons.
class TemplateRegistry {
public:
  void scan(llvm::StringRef Root);

  /// Applies an opt-in role/config preset to templates discovered by scan().
  /// An explicit name list replaces the default stc_ prefix rule; Auto adds
  /// templates whose headers contain STC configuration markers.
  void applyPreset(TemplatePreset Preset,
                   llvm::ArrayRef<llvm::StringRef> Names = {},
                   bool Auto = false);

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
      // Mangled-name lookup is for type arguments only. Function templates use
      // the explicit `name(Type, ...)` spelling so helper identifiers such as
      // `name_impl` are not treated as instantiations.
      if (Def->Kind == TemplateKind::Function)
        continue;
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
