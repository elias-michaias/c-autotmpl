<p align="center">
  <h1 align="center">c-autotmpl</h1>
  <p align="center">C template-style generic headers, generated from your editor or your build.</p>
</p>

<p align="center">
  <a href="LICENSE.md"><img src="https://img.shields.io/badge/license-MIT-blue" alt="MIT license"></a>
  <a href="https://github.com/elias-michaias/c-autotmpl/actions/workflows/cli-build.yml"><img src="https://github.com/elias-michaias/c-autotmpl/actions/workflows/cli-build.yml/badge.svg" alt="CI status"></a>
</p>

<p align="center">
  <a href="#features">Features</a> &bull;
  <a href="#example">Example</a> &bull;
  <a href="#generated-output">Generated output</a> &bull;
  <a href="#get-started">Get started</a> &bull;
  <a href="doc/USAGE.md">Usage</a> &bull;
  <a href="doc/POLYMORPHISM.md">Polymorphism</a> &bull;
  <a href="doc/BUILD.md">Build</a> &bull;
  <a href="#benchmarks">Benchmarks</a>
</p>

---

<p align="center">
  <img src="assets/demo.gif" alt="c-autotmpl generating headers live in Neovim">
</p>

c-autotmpl generates C template-style re-include headers. It runs as a small,
fast ISO C99 command line tool, or as a clangd plugin that works while you write
C code.

## Features

| Feature | Description |
| --- | --- |
| **Single-file CLI** | One ISO C99 source. No LLVM, libclang, clang headers, C++ runtime, or compile database. |
| **Editor integration** | clangd feature module generates from unsaved buffers and prunes stale blocks. |
| **Nested generics** | `List(Box(int))` and deeper chains, flattened into one header. |
| **Function templates** | `convert(int, float)(x)` and `select(int, Foo)(a, b)` from explicit call-site arguments. |
| **Lexical HKTs** | Pass a template name as an argument, as in `User(Option, int)`. |
| **Idempotent output** | Additive writes and stable markers make reruns safe in build scripts. |
| **Type-aware guards** | User types use real include guards; scalar types are emitted bare. |


## Example

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
`List(Box(int))`, and `List(Box(List(int)))`. Function templates use the same
explicit spelling: `convert(int, float)(x)`.

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

Nested dependencies are emitted in the same generated header, before the block
that needs them. A source therefore includes only the outermost header:

```c
/* c-autotmpl: Box(float) */
/* c-autotmpl: List(Box_float) */
```

The inner block is also mirrored into its own header, so direct use still works.
No call-site alias macros are generated. See [doc/USAGE.md](doc/USAGE.md) for the
full rules.

Function templates use a `function` decorator and spell every argument at the
call site. For `// -- template function convert(From, To)`, a usage of
`convert(int, float)(x)` produces:

```c
/* c-autotmpl: convert(int, float) */
#ifndef C_AUTOTMPL_CONVERT_INT__FLOAT_...
#define C_AUTOTMPL_CONVERT_INT__FLOAT_...
#define From int
#define To float
#include "../tmpl/convert.h"
#undef To
#undef From
#endif
```

Parameters are defined in decorator order and undefined in reverse order. Each
argument gets its own guard check. Helper names such as `convert_impl` are not
instantiations; there is no AST deduction.

Function headers are lexed too. After substituting the concrete arguments, a
function head such as `static inline Item sum(Item)(List(Item) *xs)` adds
`List(int)` as a dependency automatically.

A bare registered type-template name is a constructor argument. For
`// -- template type User(F, Payload)`, `User(Option, int)` defines `F` as
`Option`, lexes the substituted header, and emits `Option(int)` first.
`User(Result, int)` emits `Result(int)` instead. There is no kind checking,
only lexical application.

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

For most editors, adding that `--load` flag to the clangd command is the only setup.
The plugin is C++17 built against clangd internals, so rebuild it when clangd
changes. Full instructions are in [doc/BUILD.md](doc/BUILD.md).

## Documentation

| Document | Description |
| --- | --- |
| [**Build guide**](doc/BUILD.md) | CLI build, clangd plugin build, loading, tests, and build errors. |
| [**Usage guide**](doc/USAGE.md) | CLI options, template authoring, guards, nested types, and runtime behavior. |
| [**Polymorphism**](doc/POLYMORPHISM.md) | Generated functions, function templates, dispatch, and direct trait calls. |
| [**Troubleshooting**](doc/TROUBLESHOOTING.md) | Common failures and fixes. |

## Repository layout

```text
src/cli/      one self-contained C99 CLI source file
src/plugin/   clangd feature module and its C++ core
tools/        Python integration checks and benchmark harness
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
- Function templates need explicit type arguments; there is no deduction.
- Nested blocks depend on include order in the source file.
- The clangd module must be rebuilt against a matching clangd version.

---

<p align="center">
  <a href="LICENSE.md">MIT licensed</a> &bull;
  <a href="https://github.com/elias-michaias/c-autotmpl/issues">Report an issue</a>
</p>
