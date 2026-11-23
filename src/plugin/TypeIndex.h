#ifndef C_AUTOTMPL_TYPE_INDEX_H
#define C_AUTOTMPL_TYPE_INDEX_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <string>

namespace c_autotmpl {

/// Fast text-based map from user-defined type name to the include guard of the
/// guarded header that declares it. Used by the standalone CLI where no clang
/// AST is available.
class TypeIndex {
public:
  /// Indexes guarded headers under Root. When TypeNames is non-empty, only
  /// headers mentioning one of those names are tokenized, and the walk stops
  /// once every name has been found.
  void scan(llvm::StringRef Root,
            llvm::ArrayRef<std::string> TypeNames = {});

  std::string guardForType(llvm::StringRef TypeSpelling) const;

  /// Absolute path of the guarded header that declared this type.
  std::string headerForType(llvm::StringRef TypeSpelling) const;

  bool empty() const { return Guards.empty(); }

private:
  llvm::StringMap<std::string> Guards;
  llvm::StringMap<std::string> Headers;
};

} // namespace c_autotmpl

#endif // C_AUTOTMPL_TYPE_INDEX_H
