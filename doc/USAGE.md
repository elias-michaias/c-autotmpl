# Usage

Use the CLI for build scripts and reproducible generation. Use the clangd module
for generation from unsaved buffers, type lookup through the AST, and pruning.
Both write the same generated headers. Their rules are the same, except that the
CLI does not prune.

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
--dry-run, -n        report changes without writing
--verbose, -v        print generated blocks and counters
--                   treat the rest as file names
--help, -h           show usage
```

Inputs can be files or directories. Directories are walked in sorted order,
with files before subdirectories. Without `--root`, the CLI uses the common
ancestor of the inputs. In scripts, pass `--root .` explicitly.

A second run reports `added=0`. The CLI is additive and idempotent. It does not
remove unused blocks.

A simple build rule:

```make
.PHONY: gen
gen:
	autotmpl --root . src include
```

## clangd module

Build the module as described in [BUILD.md](BUILD.md), then pass it to clangd:

```sh
clangd --load=/absolute/path/to/libc-autotmpl.so
```

On `didOpen` and `didChange`, the module scans the unsaved text, writes missing
blocks before clangd parses the file, and prunes unused blocks after its
background workspace scan finishes. It can also find a type's declaring header
through the AST when the text index has no answer.

Environment variables:

| Variable | Effect |
| --- | --- |
| `C_AUTOTMPL_ROOT` | Force the workspace root. |
| `C_AUTOTMPL_TIMING=1` | Log phase timings. |
| `C_AUTOTMPL_TEST_VERBOSE=1` | Show plugin logs in `lsp-check`. |

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
#ifndef C_AUTOTMPL_LIST_FLOAT_...
#define C_AUTOTMPL_LIST_FLOAT_...
#define T float
#include "../tmpl/list.h"
#undef T
#endif
```

The marker is used for idempotency and pruning. The `C_AUTOTMPL_...` macro lets
the same nested instantiation appear in more than one generated header without
duplicate definitions.

Hand-written `#define T ... / #include / #undef T` blocks are recognized, so
older generated files do not immediately gain duplicate copies.

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
#ifndef C_AUTOTMPL_CONVERT_INT__FLOAT_...
#define C_AUTOTMPL_CONVERT_INT__FLOAT_...
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

Generated headers are ordinary C headers in `gen/`. You can commit them or
regenerate them. Keep the `/* c-autotmpl: ... */` markers if you want plugin
pruning to work. The plugin reloads a generated header when its mtime or size
changes, so direct edits are not overwritten from a stale cache.

If something goes wrong, start with [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

---

[README](../README.md) &bull; [Build](BUILD.md) &bull; [Usage](USAGE.md) &bull; [Troubleshooting](TROUBLESHOOTING.md)
