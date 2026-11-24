# Usage

Use the CLI for build scripts and reproducible generation. Use the clangd module
for generation from unsaved buffers, type lookup through the AST, and pruning.
Both write the same public headers, implementation sources, and makefile fragment
for split templates. Their scanner rules are the same, except that the CLI does
not prune.

Build instructions are in [BUILD.md](BUILD.md).

## CLI

```sh
cc -std=c99 -O2 src/cli/autotmpl.c -o autotmpl
autotmpl --root . --verbose src/ include/
autotmpl --root . --dry-run --verbose src/main.c
```

Options:

```text
--root DIR, -r DIR   root for template and type discovery
--stc                shorthand for --profile stc
--profile NAME, -p   opt-in preset, such as stc, stc-shared, or none
--stc-template NAME  treat NAME as an STC template; repeat for more names
--stc-auto           detect STC wrappers from template-header contents
--dry-run, -n        report changes without writing
--verbose, -v        print generated blocks and counters
--                   treat the rest as file names
--help, -h           show usage
```

Inputs can be files or directories. Directories are walked in sorted order,
with files before subdirectories. Without `--root`, the CLI uses the common
ancestor of the inputs. In scripts, pass `--root .` explicitly.

Profiles are opt-in. `--stc` is shorthand for `--profile stc`. An explicit CLI
profile wins over `C_AUTOTMPL_PROFILE`. Without either, output is unchanged.

Configuration lives in environment variables. A Makefile can source and export
an `autotmpl.env` file:

```make
-include autotmpl.env
export C_AUTOTMPL_PROFILE C_AUTOTMPL_STC_TEMPLATES C_AUTOTMPL_STC_AUTO
```

```text
# autotmpl.env
C_AUTOTMPL_PROFILE=stc
C_AUTOTMPL_STC_TEMPLATES=vec,hashmap
C_AUTOTMPL_STC_AUTO=1
```

STC roles apply to templates whose names start with `stc_`, or to an explicit
name list. The CLI accepts repeated `--stc-template NAME` arguments, and
`C_AUTOTMPL_STC_TEMPLATES` accepts a comma-separated list. An explicit name
list replaces the `stc_` prefix rule for that run; it does not change template
files. A name list with no profile selects the static `stc` profile.

`--stc-auto` and `C_AUTOTMPL_STC_AUTO=1` detect wrappers from their template
headers. An STC include or two STC configuration macros are required.
Auto-detection is off by default and can be combined with a name list.
`--profile none` still wins over all STC opt-ins.

A second run reports `added=0`. The CLI is additive and idempotent. It does not
remove unused blocks.

A simple build rule:

```make
.PHONY: gen
gen:
	autotmpl --root . src include

-include gen/impl/c-autotmpl.mk
```

The fragment appends generated implementation sources to
`C_AUTOTMPL_IMPL_SRCS`. Add that variable to your link step when you use split
templates.

## clangd module

Build the module as described in [BUILD.md](BUILD.md), then pass it to clangd:

```sh
clangd --load=/absolute/path/to/libc-autotmpl.so
```

On `didOpen` and `didChange`, the module scans the unsaved text, writes missing
public headers, implementation sources, and makefile fragments before clangd
parses the file, and prunes unused blocks after its background workspace scan
finishes. It can also find a type's declaring header through the AST when the
text index has no answer. If generation lands while clangd is already parsing,
the module suppresses the transient template-name diagnostic when the generated
include is present and its guards are satisfied. A missing include still reports
an error with a fix.

Environment variables:

| Variable | Effect |
| --- | --- |
| `C_AUTOTMPL_ROOT` | Force the workspace root. |
| `C_AUTOTMPL_PROFILE` | Select an opt-in profile, such as `stc`. |
| `C_AUTOTMPL_STC_TEMPLATES` | Comma-separated STC template names, such as `vec,hashmap`. |
| `C_AUTOTMPL_STC_AUTO=1` | Detect STC wrappers from template-header contents. |
| `C_AUTOTMPL_TIMING=1` | Log phase timings. |
| `C_AUTOTMPL_TEST_VERBOSE=1` | Show plugin logs in `lsp-check`. |

The plugin reads the same environment variables. Start clangd from an
environment that has `autotmpl.env` exported. Restart clangd after changing
configuration. Without a profile, explicit template list, or auto-detection,
generation is unchanged.

A compile database is not needed for generation, but clangd still needs correct
include paths and defines to parse the project.

## What the pattern does

A template-style header is an ordinary C header that is included once per type.
The generated block defines `T`, includes the header, then undefines `T`. The
header pastes `T` into identifiers, so one header can produce `Stack_int`,
`Stack_float`, and so on. This is preprocessor metaprogramming, not C++
templates.

A minimal version looks like this:

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

The inner `STACK_NAME_I` layer matters. Token pasting with `##` does not expand
its arguments, so the extra macro forces `T` to expand first.

Further reading:

- [David Priver - Templates in C](https://www.davidpriver.com/ctemplates.html)
- [Katelyn Doucette - Writing Generic Code in C (Template Headers)](https://katelyndoucette.com/articles/writing_generics_in_c)

## Template headers

A template is a normal header with a decorator:

```c
// -- template List(T)
#define List_IMPL(T) List_##T
#define List(T) List_IMPL(T)

#ifndef T
typedef int T;
#endif
typedef struct { T value; } List_IMPL(T);
#undef T
```

`// -- template type Box(T)` is also accepted. The two forms behave the same.
The name in the decorator chooses the generated file: `tmpl/list.h` maps to
`gen/list.h`, and other directories get a sibling `gen/` directory.

Type templates can declare several parameters, for example
`// -- template type User(F, Payload)`. Call sites spell every argument:
`User(Option, int)`. Generated blocks define parameters in decorator order and
undef them in reverse order.

Use two levels of macro indirection so the argument expands before pasting.
A single-level `List_##T` breaks for argument macros.

For a usage such as `List(float)`, the generated block looks like this:

```c
/* c-autotmpl: List(float) */
#ifndef C_AUTOTMPL_List_float
#define C_AUTOTMPL_List_float
#define T float
#include "../tmpl/list.h"
#undef T
#endif
```

The marker is used for idempotency and pruning. The `C_AUTOTMPL_...` macro lets
the same nested instantiation appear in more than one generated header without
duplicate definitions. Its name follows the marker directly, so it is
reproducible by hand. Punctuation is written as `_HH` to keep spellings distinct.

Hand-written `#define T ... / #include / #undef T` blocks are recognized, so
older generated files do not immediately gain duplicate copies.

## Split implementations

A template can put implementation code in the `#ifndef TMPL_HEADER` branch and
public declarations in the `#else` branch. The first branch is active when the
template file is opened directly, so the implementation stays visible.

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
callers belong in the second branch.

The public block wraps the re-include with the marker:

```c
/* c-autotmpl: List(int) */
#ifndef C_AUTOTMPL_List_int
#define C_AUTOTMPL_List_int
#define T int
#define TMPL_HEADER 1
#include "../tmpl/list.h"
#undef TMPL_HEADER
#undef T
#endif
```

Both generators write one grouped source per split template under
`gen/impl/list.c`. It starts with `#undef TMPL_HEADER`, includes the required
user headers, then adds one marked block per specialization. Each block
re-includes the template with `TMPL_HEADER` undefined. The fragment shown in the
build rule above adds that source to `C_AUTOTMPL_IMPL_SRCS`.

Headers without `#ifndef TMPL_HEADER` keep the original behavior. Both
generators ignore generated implementation sources during scans, so re-includes
are never fed back into either scanner.

## Function templates

A function template has a `function` decorator. The names in parentheses are
parameters for the generated block, not fixed types. The template body decides
how they are used:

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

Call sites spell out all arguments. Do not put types in the decorator. Names
such as `int` and `float` would become macro names in the generated block.

```c
#include "gen/convert.h"

int n = convert(float, int)(2.5f);
float x = convert(int, float)(7);
```

For `convert(int, float)`, the generated block defines both parameters and
undefines them in reverse order:

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

Rules:

- The call-site argument count must match the decorator. Wrong arity is ignored.
- There is no AST deduction. Write `select(int, Foo)(a, b)`, not `select(a, b)`.
- Only the exact template name starts a usage. Helpers such as `select_impl(x)`
  are ignored, including by mangled-name scanning.
- The template's own `convert(From, To)(value)` definition is skipped because its
  arguments are exactly the declared parameters.
- Each argument needs its own guard. For `select(int, Foo)`, the block is wrapped
  in `#ifdef FOO_H`. Include the type header before `gen/select.h`.
- The template header is lexed after substituting the declared parameters. A
  function head such as `sum(Item)(List(Item) *xs)` adds `List(int)` as a
  dependency when the call is `sum(int)(xs)`. The substituted self-usage is
  ignored, and discovered dependencies are emitted before the function block.

## Lexical HKTs

A bare registered type-template name is a constructor argument. It is passed as
a macro name, not as a concrete type, so it needs no include guard.

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

After substituting `F` as `Option`, c-autotmpl lexes `F(Payload)` as
`Option(int)` and emits that block before `User(Option, int)`. Writing
`User(Result, int)` emits `Result(int)` instead. Include only `gen/user.h`, just
like any other nested chain.

With one constructor parameter, `Wrap(Option)` works the same way when its
header applies `F(int)`.

This is lexical application, not kind checking. `Option` and `Result` are macro
names with whatever arity their headers define. A wrong argument count fails at
compile time. Partial application needs an explicit wrapper template.

## STC profile

STC containers are configured with macros before their headers are included. A
thin wrapper carries the usual c-autotmpl decorator and keeps the call site
parametric:

```c
/* tmpl/stc_vec.h */
// -- template stc_vec(T)
#define stc_vec_CAT_I(A, B) A##B
#define stc_vec_CAT(A, B) stc_vec_CAT_I(A, B)
#define stc_vec(T) stc_vec_CAT(stc_vec_, T)
#include <stc/cstr.h>
#include <stc/vec.h>
```

The wrapper needs only its type macro. With STC roles enabled, c-autotmpl emits
function-template adapters such as
`stc_vec_push(int)(&nums, 7)`, so the derived container name and a call helper
never appear in source. With the `stc` profile enabled, `stc_vec(Foo) v;` writes:

```c
/* c-autotmpl: stc_vec(Foo) */
#ifndef C_AUTOTMPL_stc_vec_Foo
#define C_AUTOTMPL_stc_vec_Foo
#ifdef FOO_H
#define i_type stc_vec_Foo
#define i_key Foo
#define stc_vec_push(...) \
  C_AUTOTMPL_STC_ADAPTER_CAT(stc_vec(__VA_ARGS__), _push)
/* adapters used by source */
#include "../tmpl/stc_vec.h"
#undef i_key
#undef i_type
#endif
#endif
```

Adapters are derived from call sites in the current sources. A call such as
`stc_vec_push(int)(&nums, 7)` emits the `push` adapter, and
`hashmap_insert(int, int)(&m, 7, 8)` emits `insert`. Each adapter expands the
type arguments before appending the operation, so it works for one or more type
arguments. Operations are not taken from a baked-in STC version list, so a
future STC operation works as soon as it is used in source.

`stc_vec(cstr)` writes `#define i_type stc_vec_cstr`, `#define i_key cstr`, and
`#define i_traits (c_pro_key | c_use_eq)`. Plain types get no `i_traits` define.
Trait flags stay in the preset, not in the type spelling. STC string types such
as `cstr`, `csview`, `csviewz`, and `zsview` get `c_pro_key | c_use_eq` as keys
and `c_pro_val` as values. Keys and values keep the normal type rules, so `Foo`
still needs `FOO_H`. STC-owned types such as `cstr` need no c-autotmpl guard.
STC's own implementation setup, such as `STC_CSTR_CORE` for `cstr`, remains in
the build.

Roles come from the preset, not from another decorator. `Key`, `Elem`, `T`,
`Element`, and `K` map to `i_key`. `Val`, `Value`, and `V` map to `i_val`. A
wrapper that declares `Name` or `Flags` keeps the advanced explicit form, so
those values still map to `i_type` and `i_traits`. By default, only templates
named `stc_*` receive these roles. Add names with `--stc-template` or
`C_AUTOTMPL_STC_TEMPLATES` to opt in unprefixed templates, or use one of the
auto-detection options. Everything else uses
the original macro names and `#ifndef TMPL_HEADER` split detection. The default `stc` profile is static
full-include mode. The `stc-shared` profile maps `TMPL_HEADER` to `i_header` in
public blocks and `i_implement` in grouped implementation sources.

## Type guards

Builtin and fixed-width scalar types are emitted without a type guard. For any
other type, the generated block needs a traditional guard:

```c
#ifndef FOO_H
#define FOO_H

typedef struct { int x; } Foo;

#endif
```

`#if !defined(FOO_H)` followed by `#define FOO_H` also works. The guard must be
near the top of the header. `#pragma once` has no macro that generated code can
test, so it is skipped.

The plugin can also find a declaring header through the clang AST. The CLI only
uses a text index, so keep guards traditional if you use the CLI.

## Nested types

Nested applications are expanded inside out:

```c
List(Box(int))
```

First `Box(int)` becomes `Box_int`. Then `List(Box_int)` is emitted. The chain is
flattened into the outermost generated header in dependency order:

```c
/* c-autotmpl: Box(int) */
/* c-autotmpl: List(Box_int) */
```

Because the inner block appears first in the same header, no `GEN_BOX_H` guard
is needed there. Inner blocks are also mirrored into their own headers, with the
guard restored, so direct use still works.

No call-site alias macros are generated. Write `List(Box(int))` or
`List(Box_int)` directly.

## Generated files

Generated headers are ordinary C headers in `gen/`. Split templates also get a
grouped source in `gen/impl/` and a shared `gen/impl/c-autotmpl.mk` fragment.
You can commit generated files or regenerate them. Keep the
`/* c-autotmpl: ... */` markers in headers if you want plugin pruning to work.
The plugin reloads a generated header when its mtime or size changes, so direct
edits are not overwritten from a stale cache. It applies the same pruning and
fragment update pass to generated implementation sources.

If something goes wrong, start with [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

---

[README](../README.md) &bull; [Build](BUILD.md) &bull; [Usage](USAGE.md) &bull; [Troubleshooting](TROUBLESHOOTING.md)
