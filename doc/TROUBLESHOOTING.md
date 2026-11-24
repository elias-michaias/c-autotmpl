# Troubleshooting

For plugin problems, start with the verbose log:

```sh
clangd --log=verbose --load=/absolute/path/to/libc-autotmpl.so
```

On startup you should see a `scanning ... found N template(s)` line. A little
later, `workspace scan complete` should appear. Pruning waits for that second
line.

| Symptom | Fix |
| --- | --- |
| No `[c-autotmpl]` lines | The `--load` path is wrong or clangd was already running. Use an absolute path and restart the editor. |
| `undefined symbol` at startup | The module does not match the clangd that loaded it. Rebuild with the same LLVM revision and ABI. See [BUILD.md](BUILD.md). |
| `FeatureModule.h` not found | `LLVM_SRC` or `LLVM_BUILD` points at the wrong tree. |
| `found 0 template(s)` | Check the decorator spelling: `// -- template Name(T)`, `// -- template type Name(T)`, or `// -- template function Name(P1, P2)`. |
| Wrong project root | The plugin checks `tmpl/` or `templates/`, then `.clangd`, `compile_commands.json`, and `.git`. Set `C_AUTOTMPL_ROOT` to force one. |
| CLI finds no templates | Pass `--root .` explicitly. The inferred common ancestor may be wrong. |
| `skipping Name(Foo): no traditional include guard` | Give `Foo` an `#ifndef FOO_H / #define FOO_H` guard. `#pragma once` is not supported. |
| `skipping stc_vec(Foo): no traditional include guard` | `Foo` needs a traditional `FOO_H` guard, even in STC mode. STC-owned types such as `cstr` are exempt. |
| STC roles were not applied | Enable `--stc`, `--profile stc`, or `C_AUTOTMPL_PROFILE`. The name must start with `stc_`, appear in `--stc-template` or `C_AUTOTMPL_STC_TEMPLATES`, or be found by `--stc-auto` / `C_AUTOTMPL_STC_AUTO`. |
| Unprefixed STC adapter is undefined | Add a call such as `vec_push(int)(...)` and regenerate. Adapters come from the current call sites, not from a fixed operation list. The template must be selected by name or auto-detection. |
| Derived STC name does not match the call site | Keep the wrapper paste macro in step with the template name, such as `stc_vec(T)` pasting to `stc_vec_##T`. Identifier-like type arguments are supported. |
| Adapter call creates a `Name(Fn)` warning | Update to the current build. Call-like identifiers such as `vec_push(int)` are scanned as adapter uses only when a second call parenthesis follows the type arguments. |
| Function template generated nothing | Spell every argument at the call site, e.g. `convert(int, float)(x)`. Wrong arity and helper names such as `convert_impl(x)` are ignored. |
| Function template block is skipped at compile time | Each user-defined argument needs a guard. Include its type header before the generated header. Header-discovered type dependencies are emitted into the same header for you. |
| HKT argument is treated as a type | The argument must be a bare registered template name, such as `Option`. The generated parameter is a macro name and has no include guard. |
| Usage produced only by a macro expansion | The CLI cannot see it. Use the plugin, which has an AST diagnostic fallback, or spell the usage directly. |
| Two identical blocks in a generated header | The hand-written block used a different macro parameter name, or its `C_AUTOTMPL_...` macro was removed. |
| Split template produces no object file | Include `gen/impl/c-autotmpl.mk` and link `C_AUTOTMPL_IMPL_SRCS`. Both the CLI and plugin write that fragment when a split template has usages. |
| Editor generation creates no `gen/impl/` | The template needs a lexical `#ifndef TMPL_HEADER` and a scanned usage. Open a source that uses it, then check the `[c-autotmpl]` log for `generated implementation`. |
| Duplicate types in an implementation source | Put public declarations in the `#else` branch. Keep only implementation code under `#ifndef TMPL_HEADER`. |
| Nested block exists but is inactive | Include the outermost generated header. Its flattened chain defines the inner blocks first. |
| `GEN_BOX_H` is undefined | Harmless when using the flattened chain. It still guards copies emitted into `gen/box.h`. |
| Unused blocks are not removed | Wait for `workspace scan complete`. The CLI never prunes. |
| External edit came back | The plugin caches by mtime and size. Edit or touch the file again. |
| MSVC reports `_stat` or `_access` undeclared | Rebuild with the current source. The Windows shim includes `<io.h>` and `<sys/stat.h>`. |

### Editor diagnostics in template headers

clangd also parses `tmpl/*.h` files as ordinary translation units, where the
generated parameter macros are absent. Give the header a local fallback for
that case instead of hiding the real body:

```c
#ifndef Item
#define Item int
#define T int
#include "list.h"
#undef T
#endif
```

Here `Item` becomes `int`, and including `list.h` with `T` as `int` supplies the
`List(int)` type and its trait implementation. Generated blocks define `Item`
themselves, so the fallback is skipped there. Point the fallback at the
concrete dependency your header actually names.

Run `make selftest`, `make lsp-check`, and `make standalone-check` to separate
core generation problems from clangd integration problems. Add
`C_AUTOTMPL_TEST_VERBOSE=1` to see plugin logs from `lsp-check`.

---

[README](../README.md) &bull; [Build](BUILD.md) &bull; [Usage](USAGE.md) &bull; [Troubleshooting](TROUBLESHOOTING.md)
