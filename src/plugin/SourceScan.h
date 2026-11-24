#ifndef C_AUTOTMPL_SOURCE_SCAN_H
#define C_AUTOTMPL_SOURCE_SCAN_H

#include "TemplateRegistry.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <string>

namespace c_autotmpl {

struct Token {
  llvm::StringRef Text;
  unsigned Offset;
};

struct UsageDep {
  const TemplateDef *Template;
  std::string TypeSpelling;
};

struct FoundUsage {
  const TemplateDef *Template;
  std::string TypeSpelling;
  /// Canonical individual type arguments. Function templates use one entry
  /// per declared parameter; legacy type templates use the whole spelling.
  llvm::SmallVector<std::string, 2> Args;
  /// Inner template instantiations that must exist before this block can be
  /// emitted, e.g. `Box(int)` for `List(Box(int))`.
  llvm::SmallVector<UsageDep, 2> DependsOn;
  /// True when this usage came only from expanding another application's type.
  /// Such blocks are emitted into the outermost generated header so mutually
  /// dependent nested types cannot be separated by include order.
  bool IsNested = false;
  /// Nonempty for `vec_push(int)(...)`, where push is the STC operation that
  /// must get a generated adapter macro.
  std::string AdapterOp;
};

/// Replaces comments and string/char literals with spaces while preserving
/// newlines and byte offsets.
std::string stripCommentsAndLiterals(llvm::StringRef Source);

/// Produces identifiers and single-character punctuation tokens.
void tokenize(llvm::StringRef Clean,
              llvm::SmallVectorImpl<Token> &Out);

/// Expands nested template applications in a type spelling to their mangled
/// type names (e.g. `Box(int)` -> `Box_int`) and collects the inner template
/// instantiations that must exist first. Already-mangled generic arguments
/// such as `Box_int` are recognized as dependencies too.
std::string canonicalizeType(llvm::StringRef Type,
                             const TemplateRegistry &Registry,
                             llvm::SmallVectorImpl<FoundUsage> &Deps);

/// Expands one `Template(RawType)` application into its inner dependencies
/// followed by the outer usage (so callers can generate in order). Exposed for
/// the plugin's diagnostic fallback, which sees a single type spelling rather
/// than a whole buffer.
void scanTemplateUsage(const TemplateDef &Template, llvm::StringRef RawType,
                       const TemplateRegistry &Registry,
                       llvm::SmallVectorImpl<FoundUsage> &Out,
                       bool ScanHeaders = true);

/// Finds `List(int)` and raw `List_int` usages in already-decorated source.
/// Nested template applications in type arguments are expanded and their
/// dependencies are emitted before the outer usage.
void scanUsages(llvm::StringRef Code, const TemplateRegistry &Registry,
                llvm::SmallVectorImpl<FoundUsage> &Out);

llvm::StringRef lastIdentifier(llvm::StringRef Spelling);
bool isBuiltinTypeName(llvm::StringRef Name);
bool needsIncludeGuard(llvm::StringRef TypeSpelling);

/// True when Spelling is a bare registered type-template name. Such arguments
/// are lexical constructors, not concrete types, and need no type guard.
bool isTemplateConstructorArgument(llvm::StringRef Spelling,
                                   const TemplateRegistry &Registry);

/// Splits a call's argument text into canonical top-level arguments. For a
/// type template this is one argument (the whole spelling). For a function
/// template it must match the decorator's parameter count exactly.
bool splitExplicitArgs(llvm::StringRef Raw, const TemplateDef &Template,
                       llvm::SmallVectorImpl<std::string> &Out);

/// Joins explicit arguments into the spelling used by markers and presence
/// macros, e.g. `int, Foo`.
std::string joinExplicitArgs(llvm::ArrayRef<std::string> Args);

} // namespace c_autotmpl

#endif // C_AUTOTMPL_SOURCE_SCAN_H
