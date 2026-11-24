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
  <a href="doc/USAGE.md#stc-quick-start">STC quick start</a> &bull;
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

First-class [STC](https://github.com/stclib/STC) support is opt-in. A thin
wrapper keeps `vec(int)` and `vec_push(int)(...)` call sites, and adapters are
generated from the operations your code uses. Start with
[auto-detection](doc/USAGE.md#stc-quick-start), or list template names
explicitly.

## Features

| Feature | Description |
| --- | --- |
| **Single-file CLI** | One ISO C99 source. No LLVM, libclang, clang headers, C++ runtime, or compile database. |
| **Editor integration** | clangd feature module generates from unsaved buffers and prunes stale blocks. |
| **Nested generics** | `List(Box(int))` and deeper chains, flattened into one header. |
| **Function templates** | `convert(int, float)(x)` and `select(int, Foo)(a, b)` from explicit call-site arguments. |
| **Split implementations** | `#ifndef TMPL_HEADER` selects implementation code or public declarations, with generated object sources. |
| **Lexical HKTs** | Pass a template name as an argument, as in `User(Option, int)`. |
| **Idempotent output** | Additive writes and stable markers make reruns safe in build scripts. |
| **Type-aware guards** | User types use real include guards; scalar types are emitted bare. |
| **First-class [STC](https://github.com/stclib/STC) support** | Thin wrappers and generated adapters make STC containers read like parametric types, while default templates stay unchanged. |


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

Both paths write the same public headers, grouped implementation sources, and
makefile fragment for split templates. The plugin adds editor features and
prunes unused blocks; the CLI is additive and idempotent for scripts.

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
#ifndef C_AUTOTMPL_List_float
#define C_AUTOTMPL_List_float
#define T float
#include "../tmpl/list.h"
#undef T
#endif
```

The marker drives idempotency and pruning. The presence macro lets the same
nested instantiation appear in more than one generated header without duplicate
definitions. Its name follows the marker directly, so it is reproducible by hand.
Punctuation is written as `_HH` to keep different spellings distinct.

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

A template header can put implementation code under `#ifndef TMPL_HEADER` and
public declarations in its `#else` branch. The generated public block defines
`TMPL_HEADER` before the include and undefines it afterward. Both generators
write one grouped implementation source per such template under `gen/impl/`.
Each source includes the required user headers, then re-includes the template
with `TMPL_HEADER` undefined. Both also write the fixed fragment to compile
them:

```make
-include gen/impl/c-autotmpl.mk
```

Both generators ignore generated implementation sources during scans, so editor
generation never feeds re-includes back into the scanner. Headers without
`#ifndef TMPL_HEADER` keep the original single-file behavior.

Function templates use a `function` decorator and spell every argument at the
call site. For `// -- template function convert(From, To)`, a usage of
`convert(int, float)(x)` produces:

```c
/* c-autotmpl: convert(int, float) */
#ifndef C_AUTOTMPL_convert_int_2C_20float
#define C_AUTOTMPL_convert_int_2C_20float
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

### STC with thin adapters

c-autotmpl has first-class support for [STC](https://github.com/stclib/STC).
A wrapper needs one decorator and one paste macro. The generated header adds
configuration and thin adapter macros, so calls stay native STC calls with
parametric types:

```c
vec(int) nums = {0};
vec_push(int)(&nums, 7);
```

There are two ways to opt in.

1. **Auto-detection, recommended.** Set `C_AUTOTMPL_STC_AUTO=1` or pass
   `--stc-auto`. A wrapper with an STC include or two STC configuration macros
   is detected from its template header. No name list is needed.
2. **Explicit template lists.** Set `C_AUTOTMPL_STC_TEMPLATES=vec,hashmap` or
   pass `--stc-template vec` repeatedly. This opts in unprefixed wrapper names
   that do not need auto-detection.

Both paths select the static STC profile automatically. A profile can also be
selected directly with `--stc` or `C_AUTOTMPL_PROFILE=stc`; without auto-detection
or a name list it uses the `stc_*` prefix rule. `--profile none` wins over all
STC opt-ins.

Configuration can live in an `autotmpl.env` file that your Makefile includes
and exports:

```make
-include autotmpl.env
export C_AUTOTMPL_PROFILE C_AUTOTMPL_STC_TEMPLATES C_AUTOTMPL_STC_AUTO
```

```text
# autotmpl.env, auto-detection path
C_AUTOTMPL_STC_AUTO=1

# explicit-list path instead
# C_AUTOTMPL_STC_TEMPLATES=vec,hashmap
```

Adapters come from the call sites actually used in the current sources, so a
future STC operation such as `vec_new_op(int)(...)` works without a generator
update. `stc_vec(int)` and `stc_vec(cstr)` generate derived names such as
`stc_vec_int` and `stc_vec_cstr`, then map the type parameter to `i_key`. Trait
flags stay out of the call site. STC string types such as `cstr` get
`c_pro_key | c_use_eq` as keys and `c_pro_val` as values. Plain types get no
`i_traits` define. Key and value arguments keep the normal type rules, except
STC-owned types such as `cstr` need no c-autotmpl include guard.

An explicit `Name` or `Flags` decorator parameter keeps the advanced override
form, including `i_type` and `i_traits` passed from the call site. Static
full-include mode is the default. `--profile stc-shared` also sets `i_header`
in public blocks and `i_implement` in grouped implementation sources.

The compile-checked wrapper, source, generated adapter, and build command are
in [doc/USAGE.md](doc/USAGE.md#stc-quick-start).

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
| [**STC quick start**](doc/USAGE.md#stc-quick-start) | Compile-checked STC wrappers, thin adapters, auto-detection, and explicit template lists. |
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
