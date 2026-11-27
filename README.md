<p align="center">
  <h1 align="center">c-autotmpl</h1>
  <p align="center">C template-style generic headers, generated from your editor or your build. Parametric polymorphism and static dispatch that just works in C.</p>
</p>

<p align="center">
  <a href="LICENSE.md"><img src="https://img.shields.io/badge/license-MIT-blue" alt="MIT license"></a>
  <a href="https://github.com/elias-michaias/c-autotmpl/actions/workflows/cli-build.yml"><img src="https://github.com/elias-michaias/c-autotmpl/actions/workflows/cli-build.yml/badge.svg" alt="CI status"></a>
</p>

<p align="center">
  <a href="#what-you-get">What you get</a> &bull;
  <a href="#use-it">Use it</a> &bull;
  <a href="#generic-calls">Generic calls</a> &bull;
  <a href="#configuration">Configuration</a> &bull;
  <a href="doc/USAGE.md">Usage</a> &bull;
  <a href="doc/USAGE.md#stc-quick-start">STC guide</a>
</p>

---

<p align="center">
  <img src="assets/demo.gif" alt="c-autotmpl generating headers live and dispatching generic calls in Neovim">
</p>

c-autotmpl writes the generated headers behind C template-style code such as
`List(int)` or `Box(Point)`. It runs as a single-file ISO C99 CLI for builds, or
as a clangd plugin that keeps headers current while you type. Both read the same
[project configuration](#configuration).

## What you get

- **Genericity in plain C.** Declare `List(T)` once; use `List(int)`,
  `List(Box(int))`, or deeper chains without hand-written macro blocks.
- **Generic calls.** `$vec_push(&xs, 7)` selects the concrete operation from its
  arguments at compile time, with one spelling for every instantiation.
- **Editor-aware generation.** The clangd plugin writes from unsaved buffers,
  prunes stale blocks, follows generic calls to their template declarations, and
  preserves the `$` prefix in completions.
- **Builds without toolchain baggage.** The CLI is one portable C99 source with
  no LLVM, libclang, C++ runtime, or compile database dependency.
- **STC adapters.** Use [STC](https://github.com/stclib/STC) containers as
  `vec(int)` with call sites such as `vec_push(int)(&nums, 7)` or `$vec_push(&nums, 7)`.

## At a glance

```c
#include "gen/box.h"
#include "gen/vec.h"
#include "gen/dispatch.h" /* after instantiations, before generic calls */

Box(int) box = {3};
vec(int) xs = {0};

$box_free(&box);      /* calls box_free_int */
$vec_push(&xs, 7);    /* calls vec_int_push */
```

c-autotmpl finds the template headers, writes the instantiations into a sibling
`gen/` directory, and collects the compile-time selectors into a single
`gen/dispatch.h`. Include generated instantiation headers first, then
`gen/dispatch.h`, then call sites. When several templates expose the same public
operation (for example `Option.map` and `Result.map`), their guarded selector
tables share one public spelling. A template header is just a decorator plus the
usual preprocessor pattern; see
[what the pattern does](doc/USAGE.md#what-the-pattern-does).

## Use it

### CLI

```sh
cc -std=c99 -O2 src/cli/autotmpl.c -o autotmpl
./autotmpl --root . --verbose src/ include/
./autotmpl --root . --dry-run --verbose src/main.c
```

Copy `src/cli/autotmpl.c` anywhere and compile it. Prebuilt binaries are
published by GitHub Actions.

### clangd plugin

```sh
make LLVM_SRC=/path/to/llvm-project LLVM_BUILD=/path/to/llvm-project/build
clangd --load=/absolute/path/to/build/libc-autotmpl.so
```

For most editors, the `--load` flag is the only setup. Rebuild the module when
clangd changes; see [BUILD.md](doc/BUILD.md). The plugin writes missing headers
before clangd parses a file, prunes unused blocks after its workspace scan, and
makes the editor experience match the generated code: go to definition on a
generic call opens the template function declaration, not the generated macro,
and completion keeps a `$` prefix literal instead of treating it as snippet
syntax.

Both paths use the same project options; see [Configuration](#configuration).

## Generic calls

Enable generic calls in [Configuration](#configuration) to give generated
operations one method-like spelling for every instantiation. Include
`gen/dispatch.h` after the generated instantiation headers and before the calls:

```c
vec(int) xs = {0};
vec(float) ys = {0};

$vec_push(&xs, 7);      /* vec_int_push */
$vec_push(&ys, 2.5f);   /* vec_float_push */
$vec_at(2, xs);         /* vec_int_at, value as the second argument */
$box_free(&box);        /* box_free_int, any helper spelling */
```

A type-template header contributes an operation when a function declaration's
parameter list mentions the template call form (`vec(int)`, `Box(T)`,
`pair(Key, Value)`) or a template parameter/decorator (`T`, `U`, `i_type`). The
function name is not compared with the template name, and declarations after
`#ifndef TMPL_HEADER` stay private. The public name is the function spelling
plus the configured affix, so `box_alloc(T)` becomes `$box_alloc` and selects
`box_alloc_<Type>`.

Set `dispatch_truncate=1` for bare calls such as `box_set(&box, 7)`. Declare the
operation as `box_set_impl(T)` in the template header; the dispatcher strips
`_impl` and targets `box_set_<Type>` directly. Two-argument operations use
`<op>_impl(T, U)` and target `<op>_<T>_<U>`. `dispatch_affix` is ignored in this
mode.

When another backend owns a constructor, expose it as an explicit alias in the
header preamble, for example `#define Some(T) ADT_CAT(Some, T)`. Calls use
`Some(int)(7)`; the operation stays visible to the usage scanner but gets no
payload-dispatched `_Generic` macro, so the public name is not redefined.

#### Bare call example

```text
# autotmpl.conf
dispatch=1
dispatch_truncate=1
```

```c
/* tmpl/pair.h */
// -- template type pair(T, U)
#define PAIR_CAT4_I(A, B, C, D) A##B##C##D
#define PAIR_CAT4(A, B, C, D) PAIR_CAT4_I(A, B, C, D)
#define PAIR_CAT5_I(A, B, C, D, E) A##B##C##D##E
#define PAIR_CAT5(A, B, C, D, E) PAIR_CAT5_I(A, B, C, D, E)
#define pair(T, U) PAIR_CAT4(pair_, T, _, U)
#define pair_set_impl(T, U) PAIR_CAT5(pair_set, _, T, _, U)
typedef struct { T first; U second; } pair(T, U);
static inline void pair_set_impl(T, U)(pair(T, U) *p, T a, U b) {
  p->first = a;
  p->second = b;
}
```

```c
/* main.c */
#include "gen/pair.h"
#include "gen/dispatch.h"

int main(void) {
  pair(int, float) p = {0};
  pair(float, int) q = {0};

  pair_set(&p, 3, 4.5f); /* selects pair_set_int_float */
  pair_set(&q, 1.5f, 8); /* selects pair_set_float_int */

  return p.first == 3 && p.second == 4.5f &&
                 q.first == 1.5f && q.second == 8 ? 0 : 1;
}
```

```sh
autotmpl --root . main.c
cc -I. -std=c11 -Wall -Wextra -Werror main.c -o demo
```

The public macro lives in `gen/dispatch.h`; each case is guarded by the presence
macro of an instantiation that actually exists in a generated header, and the
macro forwards arguments unchanged. Same-name operations from several templates
share one public spelling through chained owner tables. Selection uses C11
`_Generic` by default, with a GCC/Clang builtin option for C99. The affix can
lead (`$vec_push`) or trail (`vec_push_method`); all keys and defaults are in
[Configuration](#configuration).

See [Generic calls](doc/USAGE.md#generic-calls) for the association rule and
header pattern.

## STC with thin adapters

STC support is opt-in. A wrapper needs one decorator and one paste macro; the
generated header adds configuration and thin adapters so call sites read like
parametric C:

```c
vec(int) nums = {0};
vec_push(int)(&nums, 7);
```

STC can be enabled by auto-detection or an explicit template list; see
[Configuration](#configuration). Both paths select the STC profile. Adapters come
from the current call sites, so a new operation works without a generator
update. See the [STC quick start](doc/USAGE.md#stc-quick-start).

## How generation works

A template header carries a decorator:

```c
// -- template List(T)
#define List_IMPL(T) List_##T
#define List(T) List_IMPL(T)
/* ... */
```

For a use of `List(float)`, c-autotmpl appends a marked, guarded re-include
block that defines `T`, includes the template header, and undefines `T`. The
marker drives idempotency and pruning; the presence macro prevents duplicate
definitions across generated headers. User-defined types need a traditional
include guard such as `FOO_H`; builtin and fixed-width scalars are emitted bare.

For generic-call discovery, a template header can opt into local expansion with
`// -- preprocess`, or via `dispatch_preprocess_headers`. A shared header may
carry several decorators; each marked section is emitted inline with its
preamble, so related templates do not re-include the whole file. Generated
dispatch is similarly consolidated: owner sections contribute guarded selector
tables to `gen/dispatch.h`, and shared sections define the public call spelling
once even when several templates own the same operation.

Advanced generation lives in the usage guide:
[nested types](doc/USAGE.md#nested-types),
[split implementations](doc/USAGE.md#split-implementations),
[function templates](doc/USAGE.md#function-templates),
[value generics](doc/USAGE.md#value-generics), and
[lexical HKTs](doc/USAGE.md#lexical-hkts).

## Documentation

| Document | Description |
| --- | --- |
| [**Usage guide**](doc/USAGE.md) | CLI, configuration, template authoring, guards, and generated files. |
| [**Generic calls**](doc/USAGE.md#generic-calls) | Compile-time operation selection, naming, forwarding, and configuration. |
| [**STC quick start**](doc/USAGE.md#stc-quick-start) | Thin adapters, auto-detection, explicit template lists, and traits. |
| [**Polymorphism**](doc/POLYMORPHISM.md) | Generated functions, function templates, static dispatch, and direct trait calls. |
| [**Build guide**](doc/BUILD.md) | CLI build, clangd plugin build, loading, tests, and build errors. |
| [**Performance**](doc/PERFORMANCE.md) | Benchmark methodology, configuration costs, and sample results. |
| [**Troubleshooting**](doc/TROUBLESHOOTING.md) | Common failures and fixes. |

## Configuration

Project options live in `autotmpl.conf` at the workspace root. The CLI and
clangd read the same file. CLI flags override the file, the file overrides
environment variables, and environment variables override defaults. Restart
clangd after changing the file.

```text
# autotmpl.conf
profile=stc
stc_auto=1
dispatch=1
dispatch_templates=vec,pair
# dispatch_truncate=1
```

| Area | Config key | Environment variable | Effect |
| --- | --- | --- | --- |
| Profile | `profile` | `C_AUTOTMPL_PROFILE` | `stc`, `stc-shared`, or `none`. |
| STC templates | `stc_templates` | `C_AUTOTMPL_STC_TEMPLATES` | Comma-separated wrapper names, such as `vec,hashmap`. |
| STC auto | `stc_auto` | `C_AUTOTMPL_STC_AUTO` | Detect wrappers from template-header contents. |
| Generic calls | `dispatch` | `C_AUTOTMPL_DISPATCH` | Enable compile-time generic calls. |
| Call backend | `dispatch_backend` | `C_AUTOTMPL_DISPATCH_BACKEND` | `generic` (`_Generic`, default) or `builtin`. |
| Call style | `dispatch_style` | `C_AUTOTMPL_DISPATCH_STYLE` | `prefix` (default) or `postfix` affix placement. |
| Call affix | `dispatch_affix` | `C_AUTOTMPL_DISPATCH_AFFIX` | Affix text; default `$`. |
| Bare generic calls | `dispatch_truncate` | `C_AUTOTMPL_DISPATCH_TRUNCATE` | Use bare operation macros and directly mangled targets; ignore the affix. |
| Call templates | `dispatch_templates` | `C_AUTOTMPL_DISPATCH_TEMPLATES` | Comma-separated templates that get generic calls. |
| Template header preprocess for dispatch | `dispatch_preprocess_headers` | n/a | Expand selected template headers before discovering generic-call operations; `1` means all, otherwise a comma-separated template list. This is unrelated to the central `gen/dispatch.h` output. |
| Preprocess | `preprocess` | `C_AUTOTMPL_PREPROCESS` | CLI only: scan expanded source so macro-generated usages are visible. Off by default. |
| Preprocess backend | `preprocess_backend` | `C_AUTOTMPL_PREPROCESS_BACKEND` | `local` internal expander (fast) or `full` batched `cc -E -P` (default, exact). |
| Preprocessor | `preprocess_cc` | `C_AUTOTMPL_PREPROCESS_CC` | Preprocessor command for the `full` backend; default `cc`. |
| Preprocessor flags | `preprocess_flags` | `C_AUTOTMPL_PREPROCESS_FLAGS` | Extra flags such as `-Iinclude -DFEATURE=1`. |
| Plugin root | n/a | `C_AUTOTMPL_ROOT` | Force the clangd workspace root. |

Use `dispatch_style` and `dispatch_affix` for custom call spellings. With
`dispatch_truncate=1`, the affix is ignored and `<op>_impl(T, U)` declarations
produce bare `<op>` calls. Preprocessing is opt-in. The `local` backend expands
common wrapper macros inside the CLI with no external process; the `full` backend
passes sources to
batched compiler invocations and treats missing generated headers as empty
files while bootstrapping. See
[configuration in the usage guide](doc/USAGE.md#configuration) for CLI flags,
macro-generated usages, and template-authoring details.

## Build and test

```sh
make
make install
make test
make bench
```

The plugin build needs a matching LLVM source and build tree; the CLI does not.
`make install` puts the CLI at `~/.local/bin/autotmpl` and the plugin at
`~/.local/lib/c-autotmpl/libc-autotmpl.so`. Restart clangd after replacing the
module.

## Performance

It's really fast. On the benchmark workload the C99 CLI adds roughly 20 ms to
a one-second syntax check, plugin edits stay around 2 ms, and even macro-only
usages can be expanded internally in about 39 ms with `preprocess=1` and
`preprocess_backend=local`. See [PERFORMANCE.md](doc/PERFORMANCE.md) for
methodology, configuration comparisons, and `make bench` results.

## Limitations

- Generated headers always live in a sibling `gen/` directory.
- `#pragma once` headers cannot provide a testable guard.
- The CLI uses raw text by default; enable `preprocess=1` to see macro-generated usages.
- Function templates need explicit type arguments; there is no deduction.
- Nested blocks depend on include order in the source file; dispatch calls also require generated instantiation headers before `gen/dispatch.h`, which must precede the call sites.
- The clangd module must be rebuilt against a matching clangd version.

---

<p align="center">
  <a href="LICENSE.md">MIT licensed</a> &bull;
  <a href="https://github.com/elias-michaias/c-autotmpl/issues">Report an issue</a>
</p>
