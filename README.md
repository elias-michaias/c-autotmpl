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
  <a href="#get-started">Get started</a> &bull;
  <a href="doc/USAGE.md">Usage</a> &bull;
  <a href="#generated-dispatch-macros">Dispatch</a> &bull;
  <a href="doc/USAGE.md#stc-quick-start">STC guide</a>
</p>

---

<p align="center">
  <img src="assets/demo.gif" alt="c-autotmpl generating headers live in Neovim">
</p>

c-autotmpl generates C template-style re-include headers. It runs as a small,
fast ISO C99 command line tool, or as a clangd plugin that works while you write
C code.

Project settings live in `autotmpl.conf` at the workspace root, so builds and
clangd read the same short `key=value` options without environment exports.

Opt-in dispatch macros give generated functions method-like names such as
`$vec_push`. The same macro selects the right specialization from any argument,
value or pointer, and works with one or many template arguments.

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
| **Project config file** | `autotmpl.conf` gives the CLI and clangd the same `key=value` settings. |
| **Nested generics** | `List(Box(int))` and deeper chains, flattened into one header. |
| **Function templates** | `convert(int, float)(x)` from explicit call-site arguments. |
| **Split implementations** | `#ifndef TMPL_HEADER` selects a grouped implementation source and public header. |
| **Type-aware guards** | User types use real include guards; scalar types are emitted bare. |
| **Opt-in dispatch macros** | `$vec_push` and `$vec_at` dispatch from any argument, value or pointer, first or later. clangd go to definition follows the macro to its template operation. |
| **First-class [STC](https://github.com/stclib/STC) support** | Thin wrappers and generated adapters make STC containers read like parametric types. |

## Get started

### CLI

```sh
cc -std=c99 -O2 src/cli/autotmpl.c -o autotmpl
./autotmpl --root . --verbose src/ include/
./autotmpl --root . --dry-run --verbose src/main.c
```

Copy `src/cli/autotmpl.c` anywhere and compile it. It contains a small
POSIX/Win32 filesystem shim because ISO C99 has no directory API. Prebuilt
binaries are produced by GitHub Actions.

### clangd plugin

```sh
make LLVM_SRC=/path/to/llvm-project LLVM_BUILD=/path/to/llvm-project/build
clangd --load=/absolute/path/to/build/libc-autotmpl.so
```

For most editors, adding that `--load` flag is the only setup. The plugin is C++17
built against clangd internals, so rebuild it when clangd changes. See
[doc/BUILD.md](doc/BUILD.md).

Both paths write the same public headers, grouped implementation sources, and
makefile fragment for split templates. The plugin adds editor features and
pruning; the CLI is additive and idempotent for scripts.

### Project configuration

Put opt-ins in `autotmpl.conf` at the project root:

```text
# autotmpl.conf
profile=stc
stc_auto=1
dispatch=1
dispatch_templates=vec,pair
```

The CLI and clangd both read this file. CLI flags win over the file, and the
file wins over environment variables.

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

c-autotmpl finds the matching template headers and writes the instantiations into
a sibling `gen/` directory. Function templates use the same explicit spelling:
`convert(int, float)(x)`.

Template headers are a preprocessor pattern: define `T`, re-include a header
that pastes `T` into names, then undefine `T`. c-autotmpl writes those blocks for
you. [doc/USAGE.md](doc/USAGE.md#what-the-pattern-does) explains the pattern.

## Generated dispatch macros

Set `dispatch=1` in `autotmpl.conf` to get method-like names for functions
declared in type-template headers. A function qualifies when its declaration
mentions the template type spelling. It does not need a pointer, and the
container does not need to be the first argument. The same rule applies to
`vec(T)` and to two-argument templates such as `pair(Key, Value)`. Lowercase
helper bases such as `box_alloc(T)` are matched to `Box` case-insensitively and
dispatch to `box_alloc_<Type>`.

```c
#include "gen/vec.h"
#include "gen/pair.h"

vec(int) xs = {0};
vec(float) ys = {0};
pair(int, float) entry = {1, 2.5f};

$vec_push(&xs, 7);      /* selects vec_int_push */
$vec_push(&ys, 2.5f);   /* same macro, selects vec_float_push */
$vec_at(2, xs);         /* value, second argument */
$vec_at(2, ys);         /* same macro, selects vec_float_at */
$pair_sum(3, entry);    /* two-argument template */
```

The same dispatch macro selects the operation for each tracked instantiation of
a generic type. Dispatch cases come only from instantiations that exist in the
generated header. Each case is guarded by its instantiation's presence macro, so
a header never names a type that was not generated. The public macro is variadic
and forwards arguments unchanged.

With the clangd plugin, go to definition on a dispatch macro opens the matching
operation in its template header.

The default backend uses C11 `_Generic`. In `autotmpl.conf`, set
`dispatch_backend=builtin` for GCC and Clang builtins, which work in C99.
`dispatch_style=postfix` and `dispatch_affix=_method` produce `vec_push_method`;
the defaults are `prefix` and `$`. `dispatch_templates=vec,pair` limits
generation to named templates. See
[generated dispatch macros](doc/USAGE.md#generated-dispatch-macros) for the full
header pattern and configuration details.

## Generated output

A template header has a decorator:

```c
// -- template List(T)
#define List_IMPL(T) List_##T
#define List(T) List_IMPL(T)
/* ... */
```

For a usage of `List(float)`, c-autotmpl appends:

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
definitions. User-defined types need a traditional guard such as `FOO_H`;
`#pragma once` cannot be tested from generated code. Builtin and fixed-width
scalar types are emitted bare.

Advanced behavior lives in the usage guide:

- [Nested types](doc/USAGE.md#nested-types) are emitted in dependency order.
- [Split implementations](doc/USAGE.md#split-implementations) get one grouped source per template.
- [Function templates](doc/USAGE.md#function-templates) spell every argument at the call site.
- [Lexical HKTs](doc/USAGE.md#lexical-hkts) pass a template name as an argument.

## STC with thin adapters

c-autotmpl has first-class support for [STC](https://github.com/stclib/STC).
A wrapper needs one decorator and one paste macro. The generated header adds
configuration and thin adapter macros, so calls stay native STC calls with
parametric types:

```c
vec(int) nums = {0};
vec_push(int)(&nums, 7);
```

There are two ways to opt in.

1. **Auto-detection, recommended.** Set `stc_auto=1` in `autotmpl.conf` or pass
   `--stc-auto`. A wrapper with an STC include or two STC configuration macros
   is detected from its template header. No name list is needed.
2. **Explicit template lists.** Set `stc_templates=vec,hashmap` in the config or
   pass `--stc-template vec` repeatedly. This opts in unprefixed wrapper names.

Both paths select the static STC profile automatically. A profile can also be
selected directly with `--stc` or `profile=stc`; without a list or auto-detection
it uses the `stc_*` prefix rule. `--profile none` wins over all STC opt-ins.

Adapters come from current call sites, so a new operation such as
`vec_new_op(int)(...)` works without a generator update. The compile-checked
wrapper, source, generated adapter, and build command are in the
[STC quick start](doc/USAGE.md#stc-quick-start).

## Documentation

| Document | Description |
| --- | --- |
| [**Usage guide**](doc/USAGE.md) | CLI, configuration, template authoring, guards, and generated files. |
| [**Dispatch macros**](doc/USAGE.md#generated-dispatch-macros) | Association rule, argument forwarding, guard behavior, and configuration. |
| [**STC quick start**](doc/USAGE.md#stc-quick-start) | Thin adapters, auto-detection, explicit template lists, and traits. |
| [**Build guide**](doc/BUILD.md) | CLI build, clangd plugin build, loading, tests, and build errors. |
| [**Polymorphism**](doc/POLYMORPHISM.md) | Generated functions, function templates, dispatch, and direct trait calls. |
| [**Troubleshooting**](doc/TROUBLESHOOTING.md) | Common failures and fixes. |

## Build and test

```sh
make
make install
make test
make bench
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
