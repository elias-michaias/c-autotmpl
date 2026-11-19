# c-autotmpl

c-autotmpl generates C template-style re-include headers. It runs as a small,
fast ISO C99 command line tool, or as a clangd plugin that works while you write
C code.

![c-autotmpl generating headers live in Neovim](assets/demo.gif)

Given this:

```c
#include "point.h"
#include "gen/box.h"
#include "gen/list.h"

int main(void) {
    Box(Point) p = {{3}};
    List(int) xs = {7};
    List(Box(int)) nested = {{9}};
    return p.value.x + xs.value + nested.value.value;
}
```

c-autotmpl finds the matching template header and writes the instantiations into
a sibling `gen/` directory. It handles `List(int)`, `List_int`,
`List(Box(int))`, and `List(Box(List(int)))`.

There are two ways to use it:

- A standalone CLI in one C99 source file. It needs no LLVM, libclang, clang
  headers, C++ runtime, or compile database. It is meant for build scripts and
  CI, and runs in about 1 to 2 ms on the benchmark project.
- A clangd feature module built against your clangd. Add one `--load` flag and
  headers are generated from unsaved buffers. The module also uses the AST to
  find type guards and prunes unused blocks when usages disappear.

Both paths write the same generated headers. The plugin adds editor features;
the CLI is additive and idempotent for scripts.

Template-style headers are a preprocessor pattern: define `T`, re-include a
header that pastes `T` into names, then undefine `T`. c-autotmpl writes those
re-include blocks for you. [doc/USAGE.md](doc/USAGE.md#what-the-pattern-does)
explains the pattern with a small example and links to practical guides.

## Generated output

A template header has a decorator:

```c
// -- template List(T)
#define List_IMPL(T) List_##T
#define List(T) List_IMPL(T)
/* ... */
```

`// -- template type Box(T)` is also accepted. For a usage of `List(float)`,
c-autotmpl appends:

```c
/* c-autotmpl: List(float) */
#ifndef C_AUTOTMPL_LIST_FLOAT_...
#define C_AUTOTMPL_LIST_FLOAT_...
#define T float
#include "../tmpl/list.h"
#undef T
#endif
```

The marker drives idempotency and pruning. The presence macro lets the same
nested instantiation appear in more than one generated header without duplicate
definitions.

User-defined types need a traditional include guard such as `FOO_H`. Builtin and
fixed-width scalar types are emitted without a guard. `#pragma once` is not
supported because generated code cannot test it.

Nested dependencies are wrapped in the dependency header's guard:

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

No call-site alias macros are generated. The source must include `gen/box.h`
before `gen/list.h`. See [doc/USAGE.md](doc/USAGE.md) for the full rules.

## Get started

### CLI

```sh
cc -std=c99 -O2 src/cli/autotmpl.c -o autotmpl
./autotmpl --root . --verbose src/ include/
./autotmpl --root . --dry-run --verbose src/main.c
```

Copy `src/cli/autotmpl.c` anywhere and compile it. The file contains a small
POSIX/Win32 filesystem shim because ISO C99 has no directory API. Prebuilt
binaries for macOS ARM64, Linux x86-64, and Windows x86-64 are produced by
GitHub Actions.

### clangd plugin

```sh
make LLVM_SRC=/path/to/llvm-project LLVM_BUILD=/path/to/llvm-project/build
clangd --load=/absolute/path/to/build/libc-autotmpl.so
```

For Neovim, adding that `--load` flag to the clangd command is the only setup.
The plugin is C++17 built against clangd internals, so rebuild it when clangd
changes. Full instructions are in [doc/BUILD.md](doc/BUILD.md).

## Documentation

- [doc/BUILD.md](doc/BUILD.md): CLI build, clangd plugin build, loading, tests,
  and build errors. This is the longer guide.
- [doc/USAGE.md](doc/USAGE.md): CLI options, template authoring, guards, nested
  types, and runtime behavior.
- [doc/TROUBLESHOOTING.md](doc/TROUBLESHOOTING.md): common failures and fixes.

## Repository layout

```text
src/cli/      one self-contained C99 CLI source file
src/plugin/   clangd feature module and its C++ core
tools/        Python integration checks and benchmark harness
test/         small C23 fixture project
doc/          build, usage, and troubleshooting guides
assets/       demo GIF used in this README
.github/      CLI build and release workflow
```

## Build and test

```sh
make                 # plugin and CLI
make install         # install both into ~/.local
make selftest        # plugin core
make lsp-check       # live clangd session
make standalone-check
make portability-check
make test            # all checks
make bench           # overhead measurements
```

The plugin build needs a matching LLVM source and build tree. The CLI does not.
`make install` puts the CLI at `~/.local/bin/autotmpl` and the plugin at
`~/.local/lib/c-autotmpl/libc-autotmpl.so`. Restart clangd after replacing the
module.

## Benchmarks

With the C99 CLI, median of three runs:

| Measurement | 510 headers | 2010 headers |
| --- | ---: | ---: |
| `clang -fsyntax-only` | 815 ms | 813 ms |
| `autotmpl --dry-run` | 1.7 ms | 1.6 ms |
| `autotmpl` cold generation | 1.8 ms | 1.8 ms |
| plugin `prepareUpdate`, first open | 0.36 ms | 0.36 ms |
| plugin `prepareUpdate`, edits | 0.22 ms | 0.26 ms |

Run `make bench` to reproduce these numbers.

## Limitations

- Generated headers always live in a sibling `gen/` directory.
- `#pragma once` headers cannot provide a testable guard.
- The CLI does not see usages created only by macro expansion.
- Nested blocks depend on include order in the source file.
- The clangd module must be rebuilt against a matching clangd version.
