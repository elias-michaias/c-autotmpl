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
| `found 0 template(s)` | Check the decorator spelling: `// -- template Name(T)` or `// -- template type Name(T)`. |
| Wrong project root | The plugin checks `tmpl/` or `templates/`, then `.clangd`, `compile_commands.json`, and `.git`. Set `C_AUTOTMPL_ROOT` to force one. |
| CLI finds no templates | Pass `--root .` explicitly. The inferred common ancestor may be wrong. |
| `skipping Name(Foo): no traditional include guard` | Give `Foo` an `#ifndef FOO_H / #define FOO_H` guard. `#pragma once` is not supported. |
| Usage produced only by a macro expansion | The CLI cannot see it. Use the plugin, which has an AST diagnostic fallback, or spell the usage directly. |
| Two identical blocks in a generated header | The hand-written block used a different macro parameter name, or its `C_AUTOTMPL_...` macro was removed. |
| Nested block exists but is inactive | Include the dependency generated header before the outer one, for example `gen/box.h` before `gen/list.h`. |
| `GEN_BOX_H` is undefined | Same include order problem. |
| Unused blocks are not removed | Wait for `workspace scan complete`. The CLI never prunes. |
| External edit came back | The plugin caches by mtime and size. Edit or touch the file again. |
| MSVC reports `_stat` or `_access` undeclared | Rebuild with the current source. The Windows shim includes `<io.h>` and `<sys/stat.h>`. |

Run `make selftest`, `make lsp-check`, and `make standalone-check` to separate
core generation problems from clangd integration problems. Add
`C_AUTOTMPL_TEST_VERBOSE=1` to see plugin logs from `lsp-check`.
