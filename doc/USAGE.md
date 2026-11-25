# Usage

Use the CLI for build scripts and reproducible generation. Use the clangd module
for generation from unsaved buffers, type lookup through the AST, and pruning.
Both write the same public headers, implementation sources, and makefile fragment
for split templates. Build instructions are in [BUILD.md](BUILD.md).

## CLI

```sh
cc -std=c99 -O2 src/cli/autotmpl.c -o autotmpl
autotmpl --root . --verbose src/ include/
autotmpl --root . --dry-run --verbose src/main.c
```

Options:

```text
--root DIR, -r DIR   root for template and type discovery
--dry-run, -n        report changes without writing
--verbose, -v        print generated blocks and counters
--profile NAME, -p   opt-in preset, such as stc, stc-shared, or none
--stc                shorthand for --profile stc
--stc-template NAME  treat NAME as an STC template; repeat for more names
--stc-auto           detect STC wrappers from template-header contents
--                   treat the rest as file names
--help, -h           show usage
```

Inputs can be files or directories. Pass `--root .` in scripts; without it the
CLI uses the common ancestor of the inputs.

A simple build rule:

```make
.PHONY: gen
gen:
	autotmpl --root . src include

-include gen/impl/c-autotmpl.mk
```

The fragment appends generated implementation sources to
`C_AUTOTMPL_IMPL_SRCS`. Add that variable to the link step when you use split
templates. A second run reports `added=0`; the CLI is additive and idempotent,
and it does not remove unused blocks.

## Configuration

Profiles are opt-in. An explicit CLI profile wins over `C_AUTOTMPL_PROFILE`.
Without either, output is unchanged. Configuration can live in an
`autotmpl.env` file that a Makefile sources and exports:

```make
-include autotmpl.env
export C_AUTOTMPL_PROFILE C_AUTOTMPL_STC_TEMPLATES C_AUTOTMPL_STC_AUTO
export C_AUTOTMPL_DISPATCH C_AUTOTMPL_DISPATCH_BACKEND
export C_AUTOTMPL_DISPATCH_SUFFIX C_AUTOTMPL_DISPATCH_TEMPLATES
```

```text
# autotmpl.env
C_AUTOTMPL_STC_AUTO=1
```

| Variable | Effect |
| --- | --- |
| `C_AUTOTMPL_ROOT` | Force the clangd plugin workspace root. |
| `C_AUTOTMPL_PROFILE` | Select an opt-in profile, such as `stc`. |
| `C_AUTOTMPL_STC_TEMPLATES` | Comma-separated STC template names, such as `vec,hashmap`. |
| `C_AUTOTMPL_STC_AUTO=1` | Detect STC wrappers from template-header contents. |
| `C_AUTOTMPL_DISPATCH=1` | Generate method-like dispatch macros. |
| `C_AUTOTMPL_DISPATCH_BACKEND` | Select `generic` (`_Generic`, default) or `builtin` (GCC/Clang builtins). |
| `C_AUTOTMPL_DISPATCH_SUFFIX` | Set the macro suffix, default `$`. |
| `C_AUTOTMPL_DISPATCH_TEMPLATES` | Comma-separated templates that get dispatch, such as `vec,list`. |
| `C_AUTOTMPL_TIMING=1` | Log plugin phase timings. |

CLI flags take precedence over environment variables. `--profile none` wins over
every STC opt-in. STC auto-detection and explicit template lists are explained in
the [STC quick start](#stc-quick-start).

## What the pattern does

A template-style header is an ordinary C header included once per type. The
generated block defines `T`, includes the header, then undefines `T`. The header
pastes `T` into identifiers, so one header can produce `Stack_int` and
`Stack_float`. This is preprocessor metaprogramming, not C++ templates.

```c
/* tmpl/stack.h */
// -- template Stack(T)
#define STACK_NAME_I(T) Stack_##T
#define STACK_NAME(T) STACK_NAME_I(T)

#ifndef T
typedef int T;
#endif
typedef struct { T *data; int len; } STACK_NAME(T);
#undef T
```

```c
/* gen/stack.h */
#ifndef GEN_STACK_H
#define GEN_STACK_H

#define T int
#include "../tmpl/stack.h"
#undef T

#endif
```

```c
#include "gen/stack.h"

Stack_int stack = {0};
```

The extra `STACK_NAME_I` layer matters: `##` does not expand its arguments, so
the indirection forces `T` to expand first. `// -- template type Box(T)` is also
accepted and behaves the same. The decorator name chooses the generated file:
`tmpl/list.h` maps to `gen/list.h`.

Multi-parameter templates such as `// -- template type User(F, Payload)` define
arguments in decorator order and undef them in reverse order. Hand-written
`#define T ... / #include / #undef T` blocks are recognized, so older generated
files do not immediately gain duplicate copies.

Further reading:

- [David Priver - Templates in C](https://www.davidpriver.com/ctemplates.html)
- [Katelyn Doucette - Writing Generic Code in C (Template Headers)](https://katelyndoucette.com/articles/writing_generics_in_c)

## Type guards

Builtin and fixed-width scalar types are emitted without a guard. Any other type
needs a traditional guard:

```c
#ifndef FOO_H
#define FOO_H

typedef struct { int x; } Foo;

#endif
```

`#if !defined(FOO_H)` followed by `#define FOO_H` also works. The guard must be
near the top of the header. `#pragma once` has no macro that generated code can
test, so it is skipped. The plugin can also find a declaring header through the
clang AST; the CLI uses a text index.

## Nested types

Nested applications expand inside out:

```c
List(Box(int))
```

`Box(int)` becomes `Box_int`, then `List(Box_int)` is emitted. Blocks are flattened
into the outermost generated header in dependency order, and inner blocks are
mirrored into their own headers so direct use still works. No call-site alias
macros are generated; write `List(Box(int))` or `List(Box_int)` directly.

## Split implementations

A template can put implementation code in the `#ifndef TMPL_HEADER` branch and
public declarations in the `#else` branch. The first branch is active when the
template is opened directly.

```c
/* tmpl/list.h */
// -- template List(T)
#ifndef TMPL_HEADER
#define LIST_CAT_I(A, B) A##B
#define LIST_CAT(A, B) LIST_CAT_I(A, B)
#define List(T) LIST_CAT(List_, T)
T list_get(List(T) *p) { return p->value; }
#else
#define LIST_CAT_I(A, B) A##B
#define LIST_CAT(A, B) LIST_CAT_I(A, B)
#define List(T) LIST_CAT(List_, T)
typedef struct { T value; } List(T);
T list_get(List(T) *p);
#endif
```

Keep helper macros and internal types in the first branch. Public names used by
callers belong in the second branch. The generated public block defines
`TMPL_HEADER` around the re-include, and both generators write one grouped source
under `gen/impl/`. The build rule above adds that source to
`C_AUTOTMPL_IMPL_SRCS`. Headers without `#ifndef TMPL_HEADER` keep the original
single-file behavior.

## Function templates

A function template has a `function` decorator. The names in parentheses are
parameters for the generated block, not fixed types:

```c
/* tmpl/convert.h */
// -- template function convert(From, To)
#ifndef From
typedef int From;
#endif
#ifndef To
typedef int To;
#endif

#define CONVERT_CAT4_I(a, b, c, d) a##b##c##d
#define CONVERT_CAT4(a, b, c, d) CONVERT_CAT4_I(a, b, c, d)
#define convert(From, To) CONVERT_CAT4(convert_, From, _, To)

static inline To convert(From, To)(From value) { return (To)value; }

#undef To
#undef From
```

Call sites spell out all arguments:

```c
#include "gen/convert.h"

int n = convert(float, int)(2.5f);
float x = convert(int, float)(7);
```

For `convert(int, float)`, the generated block defines both parameters and
undefines them in reverse order. The call-site argument count must match the
decorator, and there is no AST deduction. Only the exact template name starts a
usage, so helpers such as `convert_impl(x)` are ignored. Each user-defined
argument needs its own guard; include its type header before the generated
header. The template header is lexed after substitution, so a function head such
as `sum(Item)(List(Item) *xs)` adds `List(int)` as a dependency when the call is
`sum(int)(xs)`.

## Lexical HKTs

A bare registered type-template name is a constructor argument passed as a macro
name, not as a concrete type, so it needs no include guard:

```c
/* tmpl/option.h */
// -- template type Option(T)
#define OPTION_CAT_I(a, b) a##b
#define OPTION_CAT(a, b) OPTION_CAT_I(a, b)
#define Option(T) OPTION_CAT(Option_, T)
typedef struct { T value; int valid; } Option(T);
#undef T
```

```c
/* tmpl/user.h */
// -- template type User(F, Payload)
#ifndef F
#define F Option
#endif
#define USER_CAT_I(a, b) a##b
#define USER_CAT(a, b) USER_CAT_I(a, b)
#define User(F, Payload) USER_CAT(User_, F)
typedef struct { F(Payload) email; } User(F, Payload);
#undef Payload
#undef F
```

```c
#include "gen/user.h"

User(Option, int) user = { { 7, 1 } };
```

After substituting `F` as `Option`, c-autotmpl emits `Option(int)` before
`User(Option, int)`. Writing `User(Result, int)` emits `Result(int)` instead.
Include only `gen/user.h`, as with any nested chain. This is lexical application,
not kind checking: a wrong argument count fails at compile time, and partial
application needs an explicit wrapper template.

## STC quick start

c-autotmpl has first-class support for [STC](https://github.com/stclib/STC)
through thin adapters. A wrapper needs one decorator and one paste macro. The
generated header adds STC configuration and adapter macros from the operations
used in your sources. The snippets below compile as shown with STC headers on
the include path.

### Auto-detection first

Set `C_AUTOTMPL_STC_AUTO=1` or pass `--stc-auto`. A wrapper that includes an STC
header is detected in `tmpl/` or `templates/`; no name list is needed.

```c
/* tmpl/vec.h */
// -- template vec(T)
#define VEC_CAT_I(A, B) A##B
#define VEC_CAT(A, B) VEC_CAT_I(A, B)
#define vec(T) VEC_CAT(vec_, T)
#include <stc/vec.h>
```

```text
# autotmpl.env
C_AUTOTMPL_STC_AUTO=1
```

```c
/* src/main.c */
#include "gen/vec.h"

#include <stdio.h>

int main(void) {
  vec(int) nums = {0};

  vec_push(int)(&nums, 7);
  vec_push(int)(&nums, 9);

  printf("%d %d\n", *vec_at(int)(&nums, 0), *vec_at(int)(&nums, 1));
  vec_drop(int)(&nums);
  return 0;
}
```

With that file sourced and exported by a Makefile, generation is
`autotmpl --root . src`. The direct shell equivalent is:

```sh
C_AUTOTMPL_STC_AUTO=1 autotmpl --root . src
cc -I. -Ithird_party/stc/include -std=c11 -Wall -Wextra -Werror \
  src/main.c -o demo
./demo
```

Replace the second `-I` with your STC include path, or drop it if STC is
installed system-wide. The program prints:

```text
7 9
```

The generated header is intentionally thin:

```c
#ifndef GEN_VEC_H
#define GEN_VEC_H

/* c-autotmpl: vec(int) */
#ifndef C_AUTOTMPL_vec_int
#define C_AUTOTMPL_vec_int
#define i_type vec_int
#define i_key int
#ifndef C_AUTOTMPL_STC_ADAPTER_CAT
#define C_AUTOTMPL_STC_ADAPTER_CAT_I(A, B) A##B
#define C_AUTOTMPL_STC_ADAPTER_CAT(A, B) C_AUTOTMPL_STC_ADAPTER_CAT_I(A, B)
#endif
#define vec_at(...) C_AUTOTMPL_STC_ADAPTER_CAT(vec(__VA_ARGS__), _at)
#define vec_drop(...) C_AUTOTMPL_STC_ADAPTER_CAT(vec(__VA_ARGS__), _drop)
#define vec_push(...) C_AUTOTMPL_STC_ADAPTER_CAT(vec(__VA_ARGS__), _push)
#include "../tmpl/vec.h"
#undef i_key
#undef i_type
#endif

#endif
```

`vec_push(int)(...)` pastes `vec(int)` to `vec_int`, then appends `_push`. Only
operations found at call sites are emitted, so a new STC operation works without
changing c-autotmpl.

### Explicit template list second

If you prefer naming wrappers, set `C_AUTOTMPL_STC_TEMPLATES=vec` or pass
`--stc-template vec`. The same wrapper and source compile with:

```sh
C_AUTOTMPL_STC_TEMPLATES=vec autotmpl --root . src
```

An explicit list opts in unprefixed names even when the wrapper has no STC
include. Without a list or auto-detection, only `stc_*` templates receive STC
roles when an STC profile is active. Both paths select the static `stc` profile
automatically.

### Roles and traits

`Key`, `Elem`, `T`, `Element`, and `K` map to `i_key`. `Val`, `Value`, and `V`
map to `i_val`. Derived names come from the call site, so `vec(int)` writes
`i_type vec_int` and `hashmap(int, int)` writes `i_type hashmap_int_int`.

STC string types such as `cstr`, `csview`, `csviewz`, and `zsview` get
`c_pro_key | c_use_eq` as keys and `c_pro_val` as values. Plain types get no
`i_traits`. STC-owned types such as `cstr` need no c-autotmpl guard; ordinary
user types still need one. STC implementation setup such as `STC_CSTR_CORE`
remains in the build.

A wrapper that declares `Name` or `Flags` keeps the advanced explicit form, where
those values map to `i_type` and `i_traits`. `--profile stc-shared` maps
`TMPL_HEADER` to `i_header` in public blocks and `i_implement` in grouped
implementation sources.

## Generated dispatch macros

Set `C_AUTOTMPL_DISPATCH=1` to emit method-like macros such as `vec_push$`. An
operation is eligible when a type-template header declares a function whose
declaration mentions that template's type spelling. The spelling can be the
template call form such as `vec(int)`, a generated argument macro such as
`i_type`, or a decorator parameter such as `Key`. Pointer-ness and argument
position do not matter. Helper functions that never mention the template type
are ignored.

The rule covers direct declarations and paste-macro declarations, including
two-argument templates such as `pair(Key, Value)`:

```c
/* tmpl/vec.h */
// -- template vec(T)
#define vec(T) vec_##T
#define VEC_FN_II(A, B) A##B
#define VEC_FN(A, B) VEC_FN_II(A, B)
typedef struct { i_key value; } i_type;
static inline void vec_int_push(i_type *v, int x) { v->value = x; }
static inline int VEC_FN(i_type, _at)(int i, i_type v) {
  (void)i;
  return v.value;
}
```

```c
/* src/main.c */
#include "gen/vec.h"

int main(void) {
  vec(int) nums = {0};
  vec_push$(&nums, 7); /* pointer, first argument */
  return vec_at$(2, nums) == 7 ? 0 : 1; /* value, second argument */
}
```

Cases come only from tracked instantiations. Each case is guarded by its
presence macro, so the header never names a type whose instantiation header was
not included. The generated macro checks up to eight call arguments and forwards
them unchanged.

```sh
C_AUTOTMPL_PROFILE=stc C_AUTOTMPL_STC_TEMPLATES=vec C_AUTOTMPL_DISPATCH=1 \
  autotmpl --root . src
cc -I. -std=c11 -Wall -Wextra -Werror src/main.c -o demo
```

The default backend is `_Generic` and needs C11. Set
`C_AUTOTMPL_DISPATCH_BACKEND=builtin` for the GCC/Clang builtin form, which also
works in C99. The default suffix `$` is accepted by GCC and Clang; set
`C_AUTOTMPL_DISPATCH_SUFFIX=_method` if you prefer an identifier suffix. Without
`C_AUTOTMPL_DISPATCH_TEMPLATES`, every eligible type template gets dispatch
macros. The plugin rereads its environment only on clangd restart.

## Generated files

Generated headers are ordinary C headers in `gen/`. Split templates also get a
grouped source in `gen/impl/` and a shared `gen/impl/c-autotmpl.mk` fragment. You
can commit generated files or regenerate them. Keep the `/* c-autotmpl: ... */`
markers if you want plugin pruning to work.

If something goes wrong, start with [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

## clangd module

Build the module as described in [BUILD.md](BUILD.md), then pass it to clangd:

```sh
clangd --load=/absolute/path/to/libc-autotmpl.so
```

On `didOpen` and `didChange`, the module scans the unsaved text and writes missing
headers, implementation sources, and makefile fragments before clangd parses the
file. It prunes unused blocks after its background workspace scan finishes and
can find a type's declaring header through the AST when the text index has no
answer.

The plugin reads the same environment variables as the CLI. Start clangd from an
environment that has `autotmpl.env` exported, and restart it after changing
configuration. A compile database is not needed for generation, but clangd still
needs correct include paths and defines to parse the project.

---

[README](../README.md) &bull; [Build](BUILD.md) &bull; [Usage](USAGE.md) &bull; [Troubleshooting](TROUBLESHOOTING.md)
