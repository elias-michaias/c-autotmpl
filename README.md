# c-autotmpl

**Automatically instantiate C “template-style” re-include headers — from a fast
standalone CLI or interactively inside clangd.**

Given source that looks like:

```c
List(int) xs;
List(Box(int)) nested;
```

...c-autotmpl finds the matching template decorator and writes the re-include
block into a sibling `gen/` header. The plugin keeps those blocks in sync as
usages come and go; the CLI is an idempotent, additive generator for scripts.
It handles one-level and nested applications such as `List(int)`,
`List(Box(int))`, `List(Box(List(int)))`, and the already-mangled spellings
`List_int` / `List(Box_int)`.

There are two ways to use it:

- **Standalone CLI**: a single, self-contained ISO C99 source file that is
  cheap enough to run on every build. No LLVM, libclang, clang headers, C++
  runtime, or compilation database.
- **clangd feature module**: built against your clangd sources and loaded with
  `clangd --load=...`, so headers are generated from unsaved buffers while you
  write C code and stale instantiations are pruned on edits.

Both paths share the same discovery, canonicalization, guard, and emission
policy; the plugin additionally provides unsaved-buffer generation, AST-based
type lookup, and pruning. They are separate implementations kept in sync by the
checks in `tools/`.

See [doc/README.md](doc/README.md) for the full documentation. In particular,
[doc/BUILD.md](doc/BUILD.md) is the main build guide: the CLI is one C99
compile, while the clangd plugin needs a matching clangd source tree and ABI.

## What it generates

A template header is marked with a decorator:

```c
// -- template List(T)
// -- template type Box(T)
#define List_IMPL(T) List_##T
#define List(T) List_IMPL(T)
/* ... */
```

The optional `type` spelling is accepted for functor-style templates; both
forms behave the same.

When the source uses `List(float)` and `float` has no instantiation yet,
c-autotmpl appends this to `gen/list.h`:

```c
/* c-autotmpl: List(float) */
#ifndef C_AUTOTMPL_LIST_FLOAT_...
#define C_AUTOTMPL_LIST_FLOAT_...
#define T float
#include "../tmpl/list.h"
#undef T
#endif
```

The `C_AUTOTMPL_...` presence macro makes repeated copies harmless when the
same nested instantiation is mirrored into more than one generated header.

For a user-defined type, the block is wrapped in that type's include guard so
the instantiation only exists once the defining header has been seen:

```c
/* c-autotmpl: List(struct Foo) */
#ifndef C_AUTOTMPL_LIST_STRUCT_FOO_...
#ifdef FOO_H
#define C_AUTOTMPL_LIST_STRUCT_FOO_...
#define T struct Foo
#include "../tmpl/list.h"
#undef T
#endif
#endif
```

Generated headers live in a sibling `gen/` directory: `tmpl/list.h` maps to
`gen/list.h`. A custom `gen/` directory is created if needed. Existing markers
and hand-written `#define T ... / #include / #undef T` blocks are recognized,
so migration does not create duplicate definitions.

## Using the standalone CLI

The entire CLI is one file:

```sh
cc -std=c99 -O2 src/cli/autotmpl.c -o autotmpl
./autotmpl --root . --verbose src/ include/
./autotmpl --root . --dry-run --verbose src/main.c
```

That is the whole dependency story: copy `src/cli/autotmpl.c` anywhere and
compile it with any C99 compiler. ISO C99 itself has no directory API, so the
same file contains a small conditionally compiled POSIX/Win32 filesystem shim;
everything above that shim is plain C99. The binary links only libc.

Prebuilt binaries for macOS ARM64, Linux x86-64, and Windows x86-64 are produced
by GitHub Actions; see [doc/CI.md](doc/CI.md).

CLI options:

- `<file-or-dir>...` — files or directories to scan; directories are walked
  recursively in sorted order, files before subdirectories;
- `--root DIR` (or `-r`) — workspace root used for template/type discovery;
  without it, a common ancestor of the inputs is used;
- `--dry-run` (or `-n`) — report what would be generated without writing;
- `--verbose` (or `-v`) — print each generated block and summary counters.

The CLI is a lexer, not a C parser. It only needs decorator comments,
`Name(Type)` / `Name_Type` usages, and traditional include guards. It does not
need a compile database or clang AST, which is why a no-op run takes about
1–2 ms on a project with 2,000 unrelated headers. libclang's C ABI would remove
C++ linkage but would still require shipping a matching libclang and building
translation units; that would be slower and less portable.

Like the plugin, the CLI:

- leaves builtin and fixed-width scalar types unguarded;
- indexes only traditionally guarded headers for the types actually used;
- skips `#pragma once` types with a one-time warning;
- schedules nested dependencies first and wraps the dependent block in the
  dependency's generated-header guard;
- never inserts `#include "box.h"` for you — see the include-order note below.

`make install` installs this binary as `~/.local/bin/autotmpl`. Run
`make portability-check` to assert that it links no LLVM, libclang, or C++
runtime.

## Using the clangd plugin

Build the plugin against the clangd source/build that matches your clangd
installation:

```sh
make
clangd --load=/absolute/path/to/build/libc-autotmpl.so ...
```

For Neovim, that is the only setup needed: add the `--load` flag to your clangd
command. There is no separate editor plugin, sidecar server, or compile-database
requirement for generation.

The module is built from `src/plugin/` as C++17 with `-fno-exceptions` and
`-fno-rtti`. It is resolved against symbols in the clangd process, so it does
not link LLVM or clangd libraries itself. Because feature modules use clangd
internals, rebuild the `.so` when you change clangd versions. The local paths
can be overridden with:

```sh
make LLVM_BUILD=... LLVM_SRC=... CLANGD_SRC=... LLVM_ABI=1
```

The full explanation—including how to obtain a matching LLVM tree, generated
header paths, compiler ABI flags, loading, and troubleshooting—is in
[doc/BUILD.md](doc/BUILD.md).

On `didOpen` and `didChange`, the plugin:

- discovers templates by walking upward from the open file, preferring `tmpl/`
  or `templates/`, then falling back to `.clangd`, `compile_commands.json`, or
  `.git`;
- generates blocks from the unsaved buffer before clangd's first parse;
- uses the clang AST to find the header that defines a user-defined type;
- prunes stale instantiations from generated headers when usages disappear;
- re-reads generated headers when their mtime changes, so direct edits in
  `gen/` are respected.

Set `C_AUTOTMPL_ROOT` only if you need to force a different workspace root. Set
`C_AUTOTMPL_TIMING=1` to log phase timings.

`make install` copies the module to
`~/.local/lib/c-autotmpl/libc-autotmpl.so`. Restart clangd/Neovim after
installing or replacing the module.

## Nested template types

Nested applications are expanded inside-out. `List(Box(int))` first emits
`Box(int)` as `Box_int`, then emits `List(Box_int)` with its whole body wrapped
in `GEN_BOX_H`:

```c
/* c-autotmpl: List(Box_int) */
#ifndef C_AUTOTMPL_LIST_BOX_INT_...
#ifdef GEN_BOX_H
#define C_AUTOTMPL_LIST_BOX_INT_...
#define T Box_int
#include "../tmpl/list.h"
#undef T
#endif
#endif
```

No call-site alias macros are generated, and no dependency `#include` is
inserted. The source must include `gen/box.h` before `gen/list.h`; otherwise
`GEN_BOX_H` is undefined and the nested block is skipped. Template headers
should use two-level paste indirection so arguments expand before pasting:

```c
#define Box_IMPL(T) Box_##T
#define Box(T) Box_IMPL(T)
```

Deeper chains such as `List(Box(List(int)))` are flattened into the outermost
generated header in dependency order:

```text
gen/list.h:  List(int), Box(List_int), List(Box_List_int)
gen/box.h:   Box(List_int) guarded by GEN_LIST_H
```

Inner blocks are also mirrored into their own headers for direct uses; their
presence macros make the duplicate copies harmless. Plugin pruning tracks both
copies, so removing the deep usage removes the inlined block too.

## Include-guard requirement

Builtin and fixed-width scalar types (`int`, `float`, `uint32_t`, ...) are
instantiated without a guard. For every other type, c-autotmpl needs a
traditional include guard:

```c
#ifndef FOO_H
#define FOO_H
/* ... */
#endif
```

The equivalent `#if !defined(FOO_H)` form also works. The guard must be followed
by a matching `#define`; unrelated conditionals near the top of a header are
ignored. The plugin finds the declaring header via the clang AST; the CLI uses
a text index over guarded headers.

`#pragma once` has no macro that generated code can test, so it is skipped with
a message such as:

```text
[c-autotmpl] skipping List(Foo): no traditional include guard found for type
```

## Repository layout

```text
src/cli/      one self-contained C99 CLI source file
src/plugin/   clangd feature module and its C++ core
tools/        Python integration checks and benchmark harness
test/         small C23 fixture project
doc/          detailed guides (build, architecture, templates, usage, CI)
.github/      CLI build/release workflow
```

`tools/standalone-check.py` exercises the CLI, `tools/lsp-check.py` drives a
real clangd session, `tools/bench.py` measures overhead, and
`src/plugin/selftest.cpp` checks the plugin core directly.

## Building and installing

```sh
make                 # plugin + portable CLI
make install         # installs both into ~/.local
make selftest        # plugin-core checks
make standalone-check
make lsp-check
make portability-check
make test            # all of the above
```

To build only the CLI, compile `src/cli/autotmpl.c` directly as shown above. To
build only the plugin, run `make build/libc-autotmpl.so` with the LLVM variables
described in [doc/BUILD.md](doc/BUILD.md). `make clean` removes all build
artifacts.

## Testing

`make test` runs:

- `selftest` — template discovery and emission, nested and deep chains,
  cross-template pruning;
- `standalone-check` — CLI idempotency, nested types, `--dry-run`, presence
  macros, guards, and warnings;
- `lsp-check` — a live clangd session using unsaved buffers, plugin generation,
  pruning, external-edit handling, and nested dependency guards;
- `portability-check` — no LLVM/libclang/C++ runtime linkage in the CLI.

Set `C_AUTOTMPL_TEST_VERBOSE=1` to print plugin log lines from `lsp-check`.

## Benchmarks

```sh
make bench
make bench BENCH_ARGS="--files 80 --headers 2000 --reps 3"
```

The harness builds a synthetic project with `Vec(TypeN)` instantiations and
unrelated headers, then measures steady-state `clang -fsyntax-only`, CLI
no-op/cold runs, clangd time to first diagnostics with and without the module,
and the plugin's internal phases.

Example with the C99 CLI (median of 3 runs):

| Measurement | 81 sources / 510 headers | 81 sources / 2010 headers |
| --- | ---: | ---: |
| `clang -fsyntax-only` | 815 ms | 813 ms |
| `autotmpl --dry-run` | 1.7 ms (0.2%) | 1.6 ms (0.2%) |
| `autotmpl` cold generation | 1.8 ms (0.2%) | 1.8 ms (0.2%) |
| c-autotmpl registry scan | 0.08 ms | 0.07 ms |
| plugin `prepareUpdate`, first open | 0.36 ms | 0.36 ms |
| plugin `prepareUpdate`, edits | 0.22 ms | 0.26 ms |
| clangd cold/warm parse overhead | within noise | within noise |

The C99 CLI uses the template-directory fast path, caches generated-header
contents within a run, and stops type indexing once all used guards are found.
The plugin scans `tmpl/` or `templates/` first and runs workspace-wide usage
scans on a background thread. First-open plugin work is roughly 1% of a cold
clangd parse and under 0.6% of a warm parse.

## Limitations

- Generated headers are always a sibling `gen/` directory; the location is not
  configurable yet.
- `#pragma once` headers cannot provide a testable guard.
- Usages produced only by macro expansion are not recognized by the textual
  CLI; the clangd path sees expanded code.
- Nested dependency blocks rely on include order in the source file.
- The clangd feature module must be rebuilt against a matching clangd version.

## Status

- [x] Discover templates from decorator comments
- [x] Generate from unsaved buffers before clangd's first parse
- [x] Prune unused instantiations on `didChange`
- [x] Generate/idempotently update generated headers
- [x] Recognize and clean up pre-existing hand-written instantiations
- [x] Notice external edits to generated headers (no stale-cache resurrection)
- [x] Guard instantiations by the type-defining header's include guard
- [x] Expand nested template applications such as `List(Box(int))`
- [x] Flatten deep chains such as `List(Box(List(int)))`
- [x] One-file portable C99 CLI with no LLVM/libclang/C++ runtime dependency
- [x] clangd feature module with minimal editor setup
- [x] Benchmark harness for CLI and clangd overhead
