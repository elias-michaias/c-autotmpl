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

First `Box(int)` becomes `Box_int`. Then `List(Box_int)` is emitted, with the
whole block wrapped in `GEN_BOX_H`:

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

The source must include the dependency generated header first:

```c
#include "gen/box.h"
#include "gen/list.h"
```

If the order is reversed, `GEN_BOX_H` is undefined and the nested block is
skipped. c-autotmpl does not insert dependency includes for you.

No call-site alias macros are generated. Write `List(Box(int))` or
`List(Box_int)` directly.

Deeper chains such as `List(Box(List(int)))` are flattened into the outermost
generated header in dependency order. Inner blocks are also mirrored into their
own headers, and the presence macros make the copies harmless.

## Generated files

Generated headers are ordinary C headers in `gen/`. You can commit them or
regenerate them. Keep the `/* c-autotmpl: ... */` markers if you want plugin
pruning to work. The plugin reloads a generated header when its mtime or size
changes, so direct edits are not overwritten from a stale cache.

If something goes wrong, start with [TROUBLESHOOTING.md](TROUBLESHOOTING.md).
