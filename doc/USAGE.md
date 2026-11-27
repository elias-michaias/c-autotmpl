# Usage

Use the CLI for build scripts and reproducible generation. Use the clangd module
for generation from unsaved buffers, type lookup through the AST, and pruning.
Both write the same public headers, the central `gen/dispatch.h`, implementation
sources, and makefile fragment for split templates. Build instructions are in
[BUILD.md](BUILD.md).

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

Profiles and generic calls are opt-in. Put project settings in `autotmpl.conf`
at the workspace root:

```text
# autotmpl.conf
profile=stc
stc_auto=1
dispatch=1
dispatch_templates=vec,pair
```

The CLI and clangd plugin both read this file. No Makefile export or `lsp.sh`
wrapper is needed. CLI flags take precedence over `autotmpl.conf`, the file takes
precedence over environment variables, and environment variables take precedence
over defaults. Restart clangd after changing configuration.

| Setting | Config key | Environment variable | Effect |
| --- | --- | --- | --- |
| Profile | `profile` | `C_AUTOTMPL_PROFILE` | Select an opt-in profile, such as `stc`. |
| STC templates | `stc_templates` | `C_AUTOTMPL_STC_TEMPLATES` | Comma-separated names, such as `vec,hashmap`. |
| STC auto | `stc_auto` | `C_AUTOTMPL_STC_AUTO` | Detect STC wrappers from template-header contents. |
| Generic calls | `dispatch` | `C_AUTOTMPL_DISPATCH` | Enable compile-time generic calls such as `$vec_push`. |
| Call backend | `dispatch_backend` | `C_AUTOTMPL_DISPATCH_BACKEND` | Select `generic` (`_Generic`, default) or `builtin` (GCC/Clang builtins). |
| Call style | `dispatch_style` | `C_AUTOTMPL_DISPATCH_STYLE` | Place the affix before the name (`prefix`, default) or after it (`postfix`). |
| Call affix | `dispatch_affix` | `C_AUTOTMPL_DISPATCH_AFFIX` | Set the generic-call affix, default `$`. |
| Bare generic calls | `dispatch_truncate` | `C_AUTOTMPL_DISPATCH_TRUNCATE` | Use bare operation macros and direct `<op>_<args>` targets; ignore the affix. |
| Call templates | `dispatch_templates` | `C_AUTOTMPL_DISPATCH_TEMPLATES` | Comma-separated templates that get generic calls, such as `vec,list`. |
| Template header preprocess for dispatch | `dispatch_preprocess_headers` | n/a | Expand template headers while discovering generic calls; `1` means all, otherwise a comma-separated template list. This is unrelated to the central `gen/dispatch.h` output. |
| Preprocess | `preprocess` | `C_AUTOTMPL_PREPROCESS` | CLI only: scan expanded source so macro-generated usages are visible. Off by default. |
| Preprocess backend | `preprocess_backend` | `C_AUTOTMPL_PREPROCESS_BACKEND` | `local` (internal expander) or `full` (batched `cc -E -P`, default). |
| Preprocessor | `preprocess_cc` | `C_AUTOTMPL_PREPROCESS_CC` | Command used by the `full` backend; default `cc`. |
| Preprocessor flags | `preprocess_flags` | `C_AUTOTMPL_PREPROCESS_FLAGS` | Extra flags such as `-Iinclude -DFEATURE=1`. |
| Plugin root | n/a | `C_AUTOTMPL_ROOT` | Force the clangd plugin workspace root. |
| Plugin timing | n/a | `C_AUTOTMPL_TIMING` | Log plugin phase timings. |

`--profile none` wins over every STC opt-in. The config file accepts comments
starting with `#`, optional spaces around `=`, and quoted values. STC
auto-detection and explicit template lists are explained in the
[STC quick start](#stc-quick-start).

### Macro-generated usages

The CLI scans raw source text by default. Enable preprocessing when a usage is
spelled only through local macros:

```text
# autotmpl.conf
preprocess=1
preprocess_backend=local
```

Two backends are available:

| Backend | Command | Cost | Fidelity |
| --- | --- | --- | --- |
| `local` | internal expander, no subprocess | ~2× raw scan | common local wrapper macros |
| `full` | batched `cc -E -P` | ~20× raw scan | compiler-exact for the selected flags |

`full` is the default so existing `preprocess=1` configurations keep their
behavior. It passes all input sources to batched preprocessor invocations,
capped at 128 files and by command-line length so Windows paths cannot overflow
`cmd.exe` limits:

```text
cc -E -P -I <workspace-root> <preprocess_flags> <source>...
```

Each source is still a separate translation unit, so macros do not leak between
files. Missing `gen/*.h` files are temporarily created empty so preprocessing
can succeed before any instantiation exists; those stubs are removed before
generated blocks are written.

`local` expands macros inside the CLI. It follows local `#include` directives
under the workspace root, honors `#define`/`#undef`, supports nested wrappers,
object-like macros, `##` token pasting, and `__VA_ARGS__`, and keeps macro state
separate per input file. It does not evaluate `#if`, system headers, built-in
target macros, or command-line function-like `-D` definitions; simple
`-DNAME=Box(int)` object macros are supported. Use `full` when a usage depends
on those features.

Both backends feed expanded text to the normal usage scanner. Operation calls
such as `box_alloc(int)()` are mapped back to their owning type template from
the header signature. Leave preprocessing off when usages are already spelled
directly.

Generic-call discovery can also expand a template header itself. Add
`// -- preprocess` to that header, or set `dispatch_preprocess_headers=1` (all
templates) or `dispatch_preprocess_headers=Option,Result` (named templates).
This is useful when operations are hidden behind macro-generated declarations.
A shared header may contain several `// -- template ...` sections; the generator
emits the preamble and each section inline, avoiding duplicate whole-header
re-includes.

The clangd plugin keeps its fast unsaved-buffer scan and does not invoke an
external preprocessor. It can recover many macro-generated type usages through
clang diagnostics after parsing.

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

## Value generics

A type template can take a value parameter as well as a type parameter. Paste a
literal constant into the generated type name; c-autotmpl recognizes numeric
arguments as values, so they need no include guard.

```c
/* tmpl/array.h */
// -- template type Array(T, N)
#define ARRAY_CAT4_I(a, b, c, d) a##b##c##d
#define ARRAY_CAT4(a, b, c, d) ARRAY_CAT4_I(a, b, c, d)
#define Array(T, N) ARRAY_CAT4(Array_, T, _, N)
typedef struct { T data[N]; } Array(T, N);
```

```c
/* main.c */
#include "gen/array.h"

int main(void) {
  Array(int, 4) a = {{1, 2, 3, 4}};
  Array(float, 2) b = {{1.0f, 2.0f}};
  return a.data[3] == 4 && b.data[1] == 2.0f ? 0 : 1;
}
```

```sh
autotmpl --root . main.c
cc -I. -std=c11 -Wall -Wextra -Werror main.c -o demo
```

The generated `gen/array.h` defines `Array_int_4` and `Array_float_2`. Each block
defines `T` and `N` before re-including `tmpl/array.h`, so `N` stays a literal in
`data[N]` while the paste macro uses the same token to form a distinct concrete
name. Named constants still work and need their usual include guard; plain
numbers do not.

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

## Generic calls

Set `dispatch=1` in `autotmpl.conf` to write method-like calls such as
`$Vec_push(&xs, 7)`. All generated selectors and public operation macros live in
one `gen/dispatch.h`. Include every generated instantiation header first, then
`gen/dispatch.h`, then the call sites. Owner sections such as `Vec` and
`Vec.push` are guarded by concrete type presence macros, and shared sections
such as `shared.push` define each public spelling exactly once. If two templates
declare the same operation name, their owner tables are chained behind that one
public spelling; the argument type chooses the owner. CLI-owned sections are
marked `generated-by=cli`, plugin-owned sections are marked
`generated-by=plugin`, and neither generator overwrites the other's sections.

The generated selector chooses the concrete operation at compile time from the
argument types. An operation is eligible when a
type-template header declares a function whose parameter list mentions that
template's call form such as
`Vec(T)`, `Pair(T, U)`, or `Box(int)`; a template parameter such as `T` or `U`;
or a generated decorator parameter such as `Key`, `i_type`, or `i_key`.
Pointer-ness and argument position do not matter. Function names are not
compared with the template name: `inspect(Box(T) *)` is eligible. Helper
functions whose parameters never mention the template are ignored, and so are
declarations inside `#ifndef TMPL_HEADER`.

The public macro is the affix plus the declared function spelling. A
`box_alloc(T)` helper therefore produces `$box_alloc` and its selector calls
`box_alloc_<Type>`. A paste-macro declaration such as `VEC_FN(i_type, _push)`
produces `<Template>_push`, so the example below yields `$Vec_push` targeting
`Vec_int_push`. Names that already contain a concrete type, such as
`Vec_int_push`, are used verbatim as the function spelling.

### Bare generic calls

With `dispatch_truncate=1`, the effective affix is none: the public macro is the
operation spelling itself. Declare the implementation as `<op>_impl(T, U)` in
the type-template header. The dispatcher strips `_impl`, and the generated macro
targets `<op>_<args>` directly. The `_impl` macro takes the template parameters
just like the function templates described earlier.

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

`gen/dispatch.h` defines the public `pair_set` macro, backed by guarded cases in
the `pair` owner section for `pair(int, float)` and `pair(float, int)`, so both
calls above use one bare spelling. Older `BOX_FN(op, T)` paste declarations and
concrete-only declarations such as `slab_set_int` remain accepted as fallbacks.
`dispatch_affix` and `dispatch_style` have no effect while truncation is on.

### Curried methods with explicit type arguments

This is the ad-hoc polymorphism form of dispatch: unrelated generated types can
own different functions with the same public operation name, and a call picks
the right implementation from its argument type. A method can also take extra
type arguments at the call site. Annotate a function-template section whose
first parameters mirror the owning type, then list the remaining parameters
after `explicit`:

```c
/* tmpl/result.h */
#define RESULT_CAT4_I(A, B, C, D) A##B##C##D
#define RESULT_CAT4(A, B, C, D) RESULT_CAT4_I(A, B, C, D)
#define MAP_CAT6_I(A, B, C, D, E, F) A##B##C##D##E##F
#define MAP_CAT6(A, B, C, D, E, F) MAP_CAT6_I(A, B, C, D, E, F)

// -- template type Result(T, U)
#define Result(T, U) RESULT_CAT4(Result_, T, _, U)
typedef struct { T ok; U err; } Result(T, U);

// -- template function map_impl(T, U, V)
// -- dispatch method Result.map explicit V
#define map_impl(T, U, V) MAP_CAT6(map_, T, _, U, _, V)
static inline Result(V, U) map_impl(T, U, V)(Result(T, U) r, V (*f)(T)) {
  Result(V, U) out = { f(r.ok), r.err };
  return out;
}
```

The generated public macro is curried. Its first call supplies the explicit
type; the second call uses ordinary dispatch to infer the owner's arguments
from the value:

```c
/* main.c */
#include "gen/result.h"
#include "gen/dispatch.h"

static float to_float(int x) { return (float)x / 2.0f; }

int main(void) {
  Result(int, float) r = {7, 0};
  Result(float, float) mapped = map(float)(r, to_float);
  return mapped.ok == 3.5f ? 0 : 1;
}
```

The target is `<op>_<owner args>_<explicit args>`, so the example calls
`map_int_float_float`. With the owner decorator below, it becomes
`<op>_<owner args>_<explicit args>_<Owner>`; `Owner` is supplied by the generator
and is not a new call-site argument. Explicit arguments are pasted into generated
names, so use single-token types such as `float`, `int`, or a typedef name. Both
the CLI and the clangd plugin understand the annotation; no source-file markers
are needed.

Two owners can share the same public spelling. Concrete functions are pasted
from their template arguments, so two owners that paste the same argument list
would collide. Add `// -- +owner` (or `// -- mangle +owner`) to a template section
to append the owning template name as the final concrete-name component. It is
opt-in: without the decorator, generated names are unchanged.

For ordinary operations, no extra implementation parameter is needed. The
generated block defines `Owner` while expanding the section. Both templates
below take one argument, yet `get(&o)` resolves to distinct functions:

```text
# autotmpl.conf
dispatch=1
dispatch_truncate=1
dispatch_templates=Box,Container,Option,Result
```

```c
/* tmpl/box.h */
// -- template type Box(T)
// -- +owner
#define BOX_CAT2_I(a, b) a##b
#define BOX_CAT2(a, b) BOX_CAT2_I(a, b)
#define BOX_CAT3_I(a, b, c) a##_##b##_##c
#define BOX_CAT3(a, b, c) BOX_CAT3_I(a, b, c)
#define Box(T) BOX_CAT2(Box_, T)
#define get_impl(T, Owner) BOX_CAT3(get, T, Owner)
typedef struct { T value; } Box(T);
static inline T get_impl(T, Owner)(Box(T) *b) { return b->value; }
#undef get_impl
```

```c
/* tmpl/container.h */
// -- template type Container(T)
// -- mangle +owner
#define CONTAINER_CAT2_I(a, b) a##b
#define CONTAINER_CAT2(a, b) CONTAINER_CAT2_I(a, b)
#define CONTAINER_CAT3_I(a, b, c) a##_##b##_##c
#define CONTAINER_CAT3(a, b, c) CONTAINER_CAT3_I(a, b, c)
#define Container(T) CONTAINER_CAT2(Container_, T)
#define get_impl(T, Owner) CONTAINER_CAT3(get, T, Owner)
typedef struct { T value; } Container(T);
static inline T get_impl(T, Owner)(Container(T) *c) { return c->value; }
#undef get_impl
```

`get(&box)` targets `get_int_Box`; `get(&container)` targets
`get_float_Container`. The public spelling and arity are unchanged.

For a curried method, add one trailing synthetic parameter to the implementation
template and its paste macro. The generator supplies the owner as that final
argument; callers still write `map(float)(o, f)`:

```c
/* tmpl/option.h */
// -- template type Option(T)
#define OPTION_CAT2_I(a, b) a##b
#define OPTION_CAT2(a, b) OPTION_CAT2_I(a, b)
#define Option(T) OPTION_CAT2(Option_, T)
typedef struct { int has; T value; } Option(T);

// -- template function option_map_impl(T, U, Owner)
// -- dispatch method Option.map explicit U
// -- +owner
#define OPTION_MAP4_I(a, b, c, d) a##_##b##_##c##_##d
#define OPTION_MAP4(a, b, c, d) OPTION_MAP4_I(a, b, c, d)
#define option_map_impl(T, U, Owner) OPTION_MAP4(map, T, U, Owner)
static inline Option(U) option_map_impl(T, U, Owner)(Option(T) value,
                                                     U (*f)(T)) {
  Option(U) out = {0};
  if (value.has) { out.has = 1; out.value = f(value.value); }
  return out;
}
```

```c
/* tmpl/result.h */
// -- template type Result(T, U)
#define RESULT_CAT3_I(a, b, c) a##_##b##_##c
#define RESULT_CAT3(a, b, c) RESULT_CAT3_I(a, b, c)
#define Result(T, U) RESULT_CAT3(Result, T, U)
typedef struct { int ok; T value; U error; } Result(T, U);

// -- template function result_map_impl(T, U, V, Owner)
// -- dispatch method Result.map explicit V
// -- mangle +owner
#define RESULT_MAP5_I(a, b, c, d, e) a##_##b##_##c##_##d##_##e
#define RESULT_MAP5(a, b, c, d, e) RESULT_MAP5_I(a, b, c, d, e)
#define result_map_impl(T, U, V, Owner) RESULT_MAP5(map, T, U, V, Owner)
static inline Result(V, U) result_map_impl(T, U, V, Owner)(Result(T, U) value,
                                                          V (*f)(T)) {
  Result(V, U) out = {0};
  if (value.ok) { out.ok = 1; out.value = f(value.value); }
  else { out.error = value.error; }
  return out;
}
```

```c
/* main.c */
#include "gen/box.h"
#include "gen/container.h"
#include "gen/option.h"
#include "gen/result.h"
#include "gen/dispatch.h"

static int twice(int x) { return 2 * x; }
static float half(int x) { return (float)x / 2.0f; }
static float scale(float x) { return 2.0f * x; }

int main(void) {
  Box(int) b = {7};
  Container(float) c = {2.5f};
  Option(int) some_int = {1, 7};
  Option(float) some_float = {1, 2.5f};
  Result(int, float) ok_int = {1, 7, 0.0f};

  int x = get(&b);                                      /* get_int_Box */
  float y = get(&c);                                    /* get_float_Container */
  Option(int) oi = map(int)(some_int, twice);           /* map_int_int_Option */
  Option(float) widened = map(float)(some_int, half);   /* map_int_float_Option */
  Option(float) of = map(float)(some_float, scale);     /* map_float_float_Option */
  Result(int, float) ri = map(int)(ok_int, twice);      /* map_int_float_int_Result */
  Result(float, float) rf = map(float)(ok_int, half);   /* map_int_float_float_Result */
  return x == 7 && y == 2.5f && oi.value == 14 && widened.value == 3.5f &&
                 of.value == 5.0f && ri.value == 14 && rf.value == 3.5f
             ? 0 : 1;
}
```

The outer argument is the explicit output type; the first value argument is the
owner discriminator. `map(float)(some_int, half)` selects
`map_int_float_Option` from `Option.map`, while `map(float)(ok_int, half)`
selects `map_int_float_float_Result` from `Result.map`. `gen/dispatch.h` emits one
`map` macro and chains the guarded owner tables. Each owner falls through to the
next when its instantiation is absent, so the two templates remain independent
and may be included in either order. The `Owner` suffix is generated only where
the decorator is present.

The rule covers direct declarations and paste-macro declarations, including
two-argument templates such as `pair(Key, Value)`:

```c
/* tmpl/vec.h */
// -- template Vec(T)
#define VEC_CAT_I(a, b) a##b
#define VEC_CAT(a, b) VEC_CAT_I(a, b)
#define Vec(T) VEC_CAT(Vec_, T)
#define VEC_FN_II(a, b) a##b
#define VEC_FN_I(a, b) VEC_FN_II(a, b)
#define VEC_FN(a, b) VEC_FN_I(a, b)
typedef struct { T data[8]; int len; } Vec(T);
static inline void VEC_FN(Vec(T), _push)(Vec(T) *v, T x) {
  v->data[v->len++] = x;
}
static inline T VEC_FN(Vec(T), _at)(int i, Vec(T) v) {
  return v.data[i];
}
```

```c
/* src/main.c */
#include "gen/vec.h"
#include "gen/dispatch.h"

int main(void) {
  Vec(int) nums = {0};
  Vec(float) floats = {0};
  $Vec_push(&nums, 7); /* selects Vec_int_push */
  $Vec_push(&floats, 2.5f); /* same macro, selects Vec_float_push */
  if ($Vec_at(0, nums) != 7) /* value, second argument */
    return 1;
  return $Vec_at(0, floats) == 2.5f ? 0 : 1;
}
```

The same generic call works for every tracked instantiation of the type. Cases
come only from tracked instantiations and are guarded by their presence macros,
so the header never names a type whose instantiation header was not included.
The public macro checks up to eight call arguments and forwards them unchanged.

```sh
autotmpl --root . src
cc -I. -std=c11 -Wall -Wextra -Werror src/main.c -o demo
```

With the clangd plugin, go to definition on a generic call opens the operation
declaration in the template header instead of the generated `#define`. No
annotation is needed in source files. When snippet completions are enabled, the
plugin escapes the configured affix in completion text, so clients insert a `$`
prefix literally instead of parsing it as snippet-variable syntax and dropping
it. In truncate mode the completion is the bare operation name instead.

The default backend is `_Generic` and needs C11. Set
`dispatch_backend=builtin` for the GCC/Clang builtin form, which also works in
C99. The default prefix `$` is accepted by GCC and Clang. Set
`dispatch_style=postfix` and `dispatch_affix=_method` for `Vec_push_method`.
Without `dispatch_templates`, every eligible type template gets generic calls.
The plugin rereads `autotmpl.conf` only on clangd restart.

## STC quick start

c-autotmpl has first-class support for [STC](https://github.com/stclib/STC)
through thin adapters. A wrapper needs one decorator and one paste macro. The
generated header adds STC configuration and adapter macros from the operations
used in your sources. The snippets below compile as shown with STC headers on
the include path.

### Auto-detection first

Set `stc_auto=1` in `autotmpl.conf` or pass `--stc-auto`. A wrapper that includes
an STC header is detected in `tmpl/` or `templates/`; no name list is needed.

```c
/* tmpl/vec.h */
// -- template vec(T)
#define VEC_CAT_I(A, B) A##B
#define VEC_CAT(A, B) VEC_CAT_I(A, B)
#define vec(T) VEC_CAT(vec_, T)
#include <stc/vec.h>
```

```text
# autotmpl.conf
stc_auto=1
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

Generation and compilation are:

```sh
autotmpl --root . src
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

If you prefer naming wrappers, set `stc_templates=vec` in `autotmpl.conf` or pass
`--stc-template vec`. The same wrapper and source compile with:

```sh
autotmpl --root . src
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

### Generic calls with STC

Generic calls are independent of the STC profile. Set `dispatch=1` with
`stc_auto=1` or `stc_templates=vec`. The same thin wrapper supports explicit
adapter calls and method-style calls:

```c
/* src/main.c */
#include "gen/vec.h"
#include "gen/dispatch.h"

#include <stdio.h>

int main(void) {
  vec(int) nums = {0};
  vec(float) floats = {0};

  vec_push(int)(&nums, 7); /* keeps the adapter in gen/ */
  $vec_push(&nums, 9); /* selects vec_int_push */
  $vec_push(&floats, 2.5f); /* same macro, selects vec_float_push */

  printf("%d %d %.1f\n", *vec_at(int)(&nums, 0), *$vec_at(&nums, 1),
         *$vec_at(&floats, 0));
  vec_drop(int)(&nums);
  vec_drop(float)(&floats);
  return 0;
}
```

```sh
autotmpl --root . src
```

One explicit call per operation is still enough to keep the adapter macro in
`gen/`. `$vec_push` and `hashmap_insert$` are public macros from shared sections
in `gen/dispatch.h`, while their guarded cases live in owner sections there and
depend on presence macros such as `C_AUTOTMPL_vec_int`; a header never names a
specialization that was not included. The same rule covers two-argument
wrappers, so `hashmap_insert$` selects from a tracked `hashmap(int, int)` case.

## Generated files

Generated headers are ordinary C headers in `gen/`. Generic-call selectors and
public macros live in the single `gen/dispatch.h`: owner sections are keyed by
template and operation (for example `vec` or `Result.map`), and shared public
sections are keyed by operation (for example `shared.push` or `shared.map`).
Every section carries a `generated-by=cli` or `generated-by=plugin` marker, and
the two generators preserve each other's sections. Split templates also get a grouped
source in `gen/impl/` and a shared `gen/impl/c-autotmpl.mk` fragment. You can
commit generated files or regenerate them. Keep the `/* c-autotmpl: ... */`
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

The plugin reads the same `autotmpl.conf` file as the CLI. Restart clangd after
changing configuration. A compile database is not needed for generation, but
clangd still needs correct include paths and defines to parse the project.

For generic calls, the plugin also routes `textDocument/definition` to the
template function declaration rather than the generated macro. It wraps
`textDocument/completion` responses to escape the configured affix in snippet
text, which keeps a `$` prefix intact in editors that parse completion snippets.

---

[README](../README.md) &bull; [Build](BUILD.md) &bull; [Usage](USAGE.md) &bull; [Troubleshooting](TROUBLESHOOTING.md)
