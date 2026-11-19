# Troubleshooting

Most problems fall into four groups: plugin loading, root/template discovery,
include guards, and nested include order. Start with the verbose plugin log:

```sh
clangd --log=verbose --load=/absolute/path/to/libc-autotmpl.so
```

Normal startup should include:

```text
[c-autotmpl] scanning /path/to/project, found N template(s)
[c-autotmpl] workspace scan complete
```

## Plugin loading

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| No `[c-autotmpl]` lines at all | `--load` path is wrong or relative; clangd was already running. | Use an absolute path and restart clangd/Neovim. |
| `dlopen failed: undefined symbol` | Module was built against a different clangd revision or ABI. | Rebuild with matching `LLVM_SRC`/`LLVM_BUILD`, `CXX`, exception/RTTI flags, and `LLVM_ABI`. See [BUILD.md](BUILD.md). |
| `dlopen failed: wrong architecture` | macOS module and clangd architectures differ. | Build with the same arch (arm64 vs Intel). |
| `FeatureModule.h` not found during build | Wrong source/build tree. | Point `LLVM_SRC` and `LLVM_BUILD` at the tree that produced your clangd. |
| Plugin loads in standalone clangd but not Neovim | Neovim starts a different clangd binary. | Check `:LspInfo` / `cmd`; use the full path to the matching clangd. |

Remember that a clangd feature module is not a stable ABI. Rebuild it whenever
clangd changes.

## Root and template discovery

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| `found 0 template(s)` | Decorator spelling is wrong. | Use exactly `// -- template Name(T)` or `// -- template type Name(T)`. |
| `found 0 template(s)` in a nested project | A nested `.clangd` or build marker hid the ancestor. | Root discovery prefers `tmpl/`/`templates/` with decorators first, but you can set `C_AUTOTMPL_ROOT`. |
| CLI cannot find templates | No `--root`, and inputs have no useful common ancestor. | Pass `--root .` explicitly. |
| Templates in hidden directories are ignored | `skipDirectory` skips dot-directories, `build/`, `node_modules/`, and `gen/`. | Move template headers to a normal directory or set a different root. |
| Root discovery misses a very deep layout | Plugin checks up to eight parent levels. | Set `C_AUTOTMPL_ROOT`. |

Verify the decorator is in a header with a `.h`, `.hh`, `.hpp`, or `.hxx`
extension.

## Generation

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| `List(float)` still undeclared after opening a file | Generation happens on `didOpen`/`didChange`; the disk file had no usages. | Type in the buffer or trigger a change; check that the plugin loaded. |
| `skipping List(Foo): no traditional include guard found` | `Foo` is defined in a `#pragma once` header or unguarded header. | Add `#ifndef FOO_H / #define FOO_H / ... / #endif`. See [TEMPLATES.md](TEMPLATES.md). |
| No generation when using a macro that expands to `List(int)` | CLI is textual and does not see macro expansion. | Use the plugin, which has an AST diagnostic fallback, or spell the usage directly. |
| `List(int)` is generated in the wrong file | Template name maps to `gen/<lowercase-name>.h`. | Rename the decorator or expected generated header accordingly. |
| Generated file is not created | Directory is not writable. | Check permissions on the `gen/` directory. |
| Plugin reports an error writing | Read-only filesystem or path collision. | Inspect the verbose log and fix the path/permissions. |

## Duplicate definitions

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| Two identical `List(float)` blocks | A hand-written block used a different macro parameter name. | Use the parameter from the decorator, usually `T`. |
| Nested type defined twice | Presence macro was stripped from a generated block. | Restore the `C_AUTOTMPL_...` guard or regenerate the header. |
| `List(Box_int)` not recognized as a dependency | `Box` template was not discovered before the usage was scanned. | Ensure both decorators are under the same root and restart clangd. |

Presence macros make mirrored copies harmless. If you see duplicate
definitions, the generated block was probably edited in a way that removed or
renamed its presence macro.

## Nested types

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| Nested block is present but disabled | Dependency generated header was included after the outer header. | Include `gen/box.h` before `gen/list.h`. |
| `GEN_BOX_H` is undefined at the nested block | Same include-order problem. | Include the dependency generated header first. |
| Tool generated a call-site alias macro | Older generated output from a previous version. | Regenerate; this version never emits alias macros. |
| Deep chain fails only in the CLI | CLI uses text guards only and has no AST fallback. | Give the type a traditional guard and check the verbose output. |

c-autotmpl intentionally does **not** insert dependency `#include` lines. See
[TEMPLATES.md](TEMPLATES.md#nested-applications).

## Pruning

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| Unused blocks are not removed yet | Full workspace scan has not completed. | Wait for `workspace scan complete` in the log; pruning starts after that. |
| Removed block comes back | Another file still uses that instantiation. | Search the workspace for the usage/marker. |
| External edit to `gen/` was overwritten | Edited before the plugin cached the file, or mtime/size did not change. | Touch/edit the file again; the plugin reloads on stamp changes. |
| CLI did not remove unused blocks | The CLI does not prune by design. | Use the clangd module, or remove blocks manually. |

## Windows CLI

| Symptom | Fix |
| --- | --- |
| `cl` reports `_access` / `_stat` undeclared | Rebuild with the current source; the Win32 shim includes `<io.h>` and `<sys/stat.h>`. |
| Security warnings from `fopen`/`fprintf` | Build with `/D_CRT_SECURE_NO_WARNINGS` (the source also defines it for MSVC). The CI workflow does this. |
| Generated include paths use `/` | Intentional: forward slashes work in MSVC includes and keep output identical across platforms. |

## Still stuck

Collect:

- `clangd --version`;
- the exact `--load` path;
- verbose clangd log around the failure;
- the decorator line and the generated header;
- the command used to build the module (including `LLVM_SRC` / `LLVM_BUILD` /
  `LLVM_ABI`).

Then run `make selftest` and `make lsp-check` to narrow down whether the core or
the clangd integration is at fault.
