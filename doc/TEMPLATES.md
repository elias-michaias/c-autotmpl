# Writing template headers

c-autotmpl does not require a new language. A “template” is an ordinary header
with a decorator comment and a macro that pastes the type argument into a
generated identifier.

## Decorator syntax

Put one of these comments in the header:

```c
// -- template List(T)
// -- template type Box(T)
```

Both forms are accepted; `type` is just an optional spelling. `T` is the macro
parameter name used in the generated block. If you use a different parameter
name, list it in the decorator:

```c
// -- template Map(Key)
#define T Key
```

The first listed parameter is the one redefined in the generated block.

A decorated header at `tmpl/list.h` maps to `gen/list.h`. A decorated header at
`include/my_list.h` maps to `include/gen/my_list.h`. The generated filename is
the lowercased template name plus `.h`.

## Macro indirection

Use two levels of paste so the argument expands before token pasting:

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

A single-level `#define List(T) List_##T` would paste the literal argument token
and break for macro arguments such as `uint32_t`.

The generated block re-includes this header with `T` defined:

```c
/* c-autotmpl: List(float) */
#ifndef C_AUTOTMPL_LIST_FLOAT_...
#define C_AUTOTMPL_LIST_FLOAT_...
#define T float
#include "../tmpl/list.h"
#undef T
#endif
```

The relative include path is computed from the generated header back to the
decorated header.

## Usages

The scanner recognizes:

```c
List(int) xs;          /* call-style */
List_int ys;           /* already-mangled style */
List(Box(int)) nested; /* nested application */
```

Comments and string literals are ignored.

## Guards for user-defined types

Builtin and fixed-width scalar types are emitted without a guard:

```text
void bool char short int long float double signed unsigned _Bool
size_t ssize_t int8_t uint8_t int16_t uint16_t int32_t uint32_t
int64_t uint64_t intptr_t uintptr_t
```

For any other type, c-autotmpl needs a traditional include guard so generated
code can test whether the type has been declared:

```c
#ifndef FOO_H
#define FOO_H

typedef struct { int x; } Foo;

#endif
```

The equivalent form also works:

```c
#if !defined(FOO_H)
#define FOO_H
/* ... */
#endif
```

The guard must be near the top and followed by a matching `#define` within the
next few lines. Unrelated conditionals are ignored. `#pragma once` has no macro
to test, so it is unsupported:

```text
[c-autotmpl] skipping List(Foo): no traditional include guard found for type
```

The plugin can also find a declaring header via the clang AST if the text index
does not. The CLI only uses the text index, so keep guards traditional for CLI
users.

## Nested applications

Nested applications are expanded inside-out. Given:

```c
List(Box(int))
```

c-autotmpl emits, in order:

1. `Box(int)` as `Box_int`;
2. `List(Box_int)` guarded by the generated `box.h` file guard.

The outer block looks like this:

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

The whole block is wrapped, not just the `#include`. That way the instantiation
does not exist unless the dependency header has already been included.

### Include order matters

c-autotmpl never inserts dependency includes for you. Your source must include
the dependency generated header first:

```c
#include "gen/box.h"   /* defines GEN_BOX_H */
#include "gen/list.h"  /* nested block is now active */
```

If the order is reversed, `GEN_BOX_H` is undefined and the nested block is
skipped. This is intentional: the tool does not guess where your headers should
be included.

### No call-site alias macros

Do not expect the tool to generate macros such as:

```c
#define List_Box(T) List_Box_##T
```

Instead, either write the nested application directly:

```c
List(Box(int)) x;
```

or spell the already-mangled type:

```c
List(Box_int) x;
```

Both canonicalize to the same instantiation.

### Deep chains

`List(Box(List(int)))` is flattened into the outermost generated header in
dependency order:

```text
gen/list.h:  List(int), Box(List_int), List(Box_List_int)
gen/box.h:   Box(List_int) guarded by GEN_LIST_H
```

Inner blocks are also mirrored into their own generated headers for direct
uses. Presence macros make the duplicate copies harmless.

## Manual blocks and migration

Hand-written blocks are recognized:

```c
#define T float
#include "../tmpl/thing.h"
#undef T
```

If the type matches a wanted instantiation, c-autotmpl will not append a second
copy. Marked blocks are preferred for new output because they also drive
pruning and cross-template nesting.

Direct edits inside `gen/` are respected. The plugin reloads a generated header
when its mtime or size changes instead of restoring a stale cached copy.

## Checklist

- Decorator comment is present and spelled exactly `// -- template ...`.
- Macro uses two-level paste indirection.
- User-defined argument types have traditional include guards.
- Nested dependency generated headers are included before the outer header.
- Generated `gen/` files are included from the source that uses the template.
