<p align="center">
  <h1 align="center">Polymorphism with template headers</h1>
  <p align="center">Checked constraints, extendible dispatch, and trait impls generated from one C header.</p>
</p>

<p align="center">
  <a href="LICENSE.md"><img src="https://img.shields.io/badge/license-MIT-blue" alt="MIT license"></a>
  <a href="https://github.com/elias-michaias/c-autotmpl/actions/workflows/cli-build.yml"><img src="https://github.com/elias-michaias/c-autotmpl/actions/workflows/cli-build.yml/badge.svg" alt="CI status"></a>
</p>

<p align="center">
  <a href="README.md">Home</a> &bull;
  <a href="doc/USAGE.md">Usage</a> &bull;
  <a href="doc/BUILD.md">Build</a> &bull;
  <a href="#checked-constraints">Constraints</a> &bull;
  <a href="#extendible-generic-dispatch">Dispatch</a> &bull;
  <a href="#c-trait-generic-impls">Traits</a>
</p>

---

A template header is included once per type. c-autotmpl writes the
`#define T` and `#include` block, so the header can do more than paste a type
into a typedef. It can check the type, register a dispatch entry, or implement a
trait for that type.

Every example below is complete. Build the CLI from this repo, put it on your
`PATH`, then run the commands shown with each example.

- [Checked constraints](#checked-constraints)
- [Extendible generic dispatch](#extendible-generic-dispatch)
- [c-trait generic impls](#c-trait-generic-impls)
- [Other contenders](#other-contenders)

## Checked constraints

A template header is still a header, so it should parse on its own in an editor.
The `#ifndef T` fallback gives tools a concrete type. Generated code defines `T`
before the include, so the fallback never reaches a real instantiation.

`_Static_assert` then checks each `T` at compile time. Here `Pair(T)` accepts a
type that fits in a pointer and is not over-aligned.

```c
/* tmpl/pair.h */
// -- template Pair(T)

#ifndef T
typedef struct { int x; int y; } T;
#endif

_Static_assert(sizeof(T) <= sizeof(void *), "Pair(T): T must fit in a pointer");
_Static_assert(_Alignof(T) <= _Alignof(void *), "Pair(T): T must not be over-aligned");

#define PAIR_CAT_I(a, b) a##b
#define PAIR_CAT(a, b) PAIR_CAT_I(a, b)
#define Pair(T) PAIR_CAT(Pair_, T)

typedef struct { T first; T second; } Pair(T);

#undef T
```

```c
/* point.h */
#ifndef POINT_H
#define POINT_H

typedef struct { int x; int y; } Point;

#endif
```

```c
/* big.h */
#ifndef BIG_H
#define BIG_H

typedef struct { char pad[64]; } Big;

#endif
```

```c
/* src/main.c */
#include "point.h"
#include "gen/pair.h"
#include <stdio.h>

int main(void) {
  Pair(int) a = { 3, 4 };
  Pair(Point) b = { {1, 2}, {5, 6} };

  printf("%d %d %d\n", a.first + a.second,
         b.first.x + b.second.x, b.first.y + b.second.y);
  return 0;
}
```

```c
/* src/bad.c */
#include "big.h"
#include "gen/pair.h"

Pair(Big) bad = {0};

int main(void) { return (int)sizeof(bad); }
```

```sh
autotmpl --root . src/main.c src/bad.c
cc -std=c11 -I. src/main.c -o demo && ./demo
# 7 6 8
cc -std=c11 -I. -fsyntax-only src/bad.c
# error: static assertion failed: "Pair(T): T must fit in a pointer"
```

The guard on `Point` matters. c-autotmpl wraps user types in the header's real
include guard so the generated `T` is only used after that type is visible.
`#pragma once` cannot be tested from generated code.

## Extendible generic dispatch

C11 `_Generic` gives static dispatch, but its association list is fixed where
the macro is defined. Jackson Allan's CC.h article describes a way to leave
empty slots in that list and let users fill them with type and function pairs.
The technique is the basis for CC's comparison, hash, and destructor dispatch.

- [Better C Generics Part 1: The Extendible `_Generic`](https://github.com/JacksonAllan/CC/blob/main/articles/Better_C_Generics_Part_1_The_Extendible_Generic.md)
- [Convenient Containers](https://github.com/JacksonAllan/CC)

The shorter slotted form below uses three registration slots. A template header
turns each `Register(T)` usage into one registration, so adding a type is the
same edit as adding a container instantiation.

```c
/* hash.h */
#ifndef HASH_H
#define HASH_H
#include <stddef.h>

#define CAT_I(a, b) a##b
#define CAT(a, b) CAT_I(a, b)

#define COMMA() ,
#define ARG_2_(_1, _2, ...) _2
#define ARG_2(...) ARG_2_(__VA_ARGS__)
#define INCL(...) __VA_ARGS__
#define OMIT(...)
#define IF_DEF(macro) ARG_2(COMMA macro () INCL, OMIT, )

#define hash(val) _Generic((val),                  \
  IF_DEF(HASH_1)(hash_1_ty: hash_1_fn, )           \
  IF_DEF(HASH_2)(hash_2_ty: hash_2_fn, )           \
  IF_DEF(HASH_3)(hash_3_ty: hash_3_fn, )           \
  default: _Generic((val),                         \
    int: hash_int, default: NULL                   \
  )                                                \
)(val)

static inline size_t hash_int(int v) { return (size_t)v * 2654435761u; }

#define ARG_1_(_1, ...) _1
#define ARG_1(...) ARG_1_(__VA_ARGS__)
#define DEF_HASH_N_TY_FN(n)                         \
typedef ARG_1(HASH) hash_##n##_ty;                  \
static inline size_t hash_##n##_fn(hash_##n##_ty v) \
{ return ARG_2(HASH, )(v); }

#endif
```

```c
/* add_hash.h */
#ifndef HASH
#error Define HASH as type, function_name before including add_hash.h
#endif

#if !defined(HASH_1)
#define HASH_1
DEF_HASH_N_TY_FN(1)
#elif !defined(HASH_2)
#define HASH_2
DEF_HASH_N_TY_FN(2)
#elif !defined(HASH_3)
#define HASH_3
DEF_HASH_N_TY_FN(3)
#else
#error Too many hash registrations
#endif

#undef HASH
```

```c
/* tmpl/register.h */
// -- template Register(T)

#define REGISTER_CAT_I(a, b) a##b
#define REGISTER_CAT(a, b) REGISTER_CAT_I(a, b)
#define Register(T) REGISTER_CAT(Register_, T)

#define HASH T, CAT(hash_, T)
#include "../add_hash.h"

typedef struct { int slot; } Register(T);

#undef T
```

```c
/* src/main.c */
#include "hash.h"

static size_t hash_long(long v) { return (size_t)v * 2654435761u; }
static size_t hash_double(double v) {
  unsigned char *p = (unsigned char *)&v;
  size_t h = 1469598103934665603u;
  int i;
  for (i = 0; i < (int)sizeof(v); ++i) {
    h ^= p[i];
    h *= 1099511628211u;
  }
  return h;
}

#include "gen/register.h"

#include <stdio.h>

int main(void) {
  Register(long) r1 = {1};
  Register(double) r2 = {2};
  printf("%zu %zu %zu %zu %d %d\n",
         hash(7), hash(2.5), hash(9L), hash(3.5),
         r1.slot, r2.slot);
  return 0;
}
```

```sh
autotmpl --root . src/main.c
cc -std=gnu11 -I. src/main.c -o demo && ./demo
```

The hash calls for `long` and `double` select the generated slots. The `int`
call falls through to `hash_int`. An unregistered type selects `NULL` in the
nested `_Generic`, which gives a call error at compile time. The article's
counter version grows this to 511 slots without listing them by hand.

Include `hash.h` and define the hash functions before `gen/register.h`. The
generated registrations are preprocessor state for the current translation
unit, so this example uses one source file.

## c-trait generic impls

[c-trait](https://github.com/elias-michaias/c-trait) uses `_Generic` to turn a
trait call into a direct call to the concrete implementation. Its associated
type pattern lets one trait keep a single method signature across generated
types. That matches a template header well: generate the struct and the trait
impl from the same `T`.

Copy `trait.h` from c-trait into the project root. No `dynamic(Self)` marker is
used, so dispatch is static and there is no vtable.

```c
/* point.h */
#ifndef POINT_H
#define POINT_H
typedef struct { int x; int y; } Point;
#endif
```

```c
/* tmpl/list.h */
// -- template List(T)

#include "../trait.h"

#ifndef T
typedef int T;
#endif

#ifndef LIST_TRAIT_DEFINED
#define LIST_TRAIT_DEFINED
#define ListSignature(Self)                        \
  required(Self, void, push, List_Item)            \
  required(Self, List_Item, get, int)
#define Trait List
#include "../trait.h"
#endif

#define LIST_CAT_I(a, b) a##b
#define LIST_CAT(a, b) LIST_CAT_I(a, b)
#define List(T) LIST_CAT(List_, T)

typedef struct { T data[8]; int len; } List(T);

#define For List(T)
#define Impl List
#define List_Item T
  void def(push, T value) { self->data[self->len++] = value; }
  T def(get, int index) { return self->data[index]; }
#include "../trait.h"
#undef List_Item
#undef Impl
#undef For

#undef T
```

```c
/* src/main.c */
#include "point.h"
#include "gen/list.h"
#include <stdio.h>

int main(void) {
  List(int) xs = {0};
  List(float) fs = {0};
  List(Point) ps = {0};

  int new_x = 7;
  float new_f = 3.5f;
  Point new_p = { 4, 5 };

  $(List.push, &xs, new_x);
  $(List.push, &xs, 9);
  $(List.push, &fs, new_f);
  $(List.push, &ps, new_p);

  printf("xs=%d,%d fs=%.1f pt=(%d,%d) sum=%d\n",
         $(List.get, &xs, 0), $(List.get, &xs, 1),
         $(List.get, &fs, 0),
         $(List.get, &ps, 0).x, $(List.get, &ps, 0).y,
         $(List.get, &xs, 0) + $(List.get, &xs, 1));
  return 0;
}
```

```sh
cp /path/to/c-trait/trait.h .
autotmpl --root . src/main.c
cc -std=gnu11 -I. src/main.c -o demo && ./demo
# xs=7,9 fs=3.5 pt=(4,5) sum=16
```

`List_Item` is set to `T` inside each generated block. The trait signature has
one `push` method, while the generated impl uses the right argument type for
`List(int)`, `List(float)`, and `List(Point)`. The `$()` call site stays the
same for all three:

```c
$(List.push, &xs, new_x);
```

Add a trait impl by adding a `List(T)` usage. c-autotmpl writes the generated
block, and the template writes the associated type and impl for that `T`.

See the [c-trait API reference](https://github.com/elias-michaias/c-trait/blob/main/docs/API.md)
for defaults, dynamic dispatch, and supertraits.

## Other contenders

- [c-match](https://github.com/elias-michaias/c-match) can pattern match over
  generated tagged unions. Let `T` choose the payload type and generate the
  tag enum and constructors from one header.
- [Intrusive linked lists](https://www.data-structures-in-practice.com/intrusive-linked-lists/)
  need the element type to supply a link field. A template can check for that
  field with a small macro and emit typed wrappers around the shared list code.
- X-macro registries can be wrapped in a template so each `T` contributes an
  enum tag, a name string, and a switch arm from one source list.
- Policy templates can take more than one parameter. For example,
  `// -- template Vec(T, Alloc)` can check `Alloc` through a generated macro and
  keep allocation out of the element type.

---

<p align="center">
  <a href="README.md">Home</a> &bull;
  <a href="doc/USAGE.md">Usage</a> &bull;
  <a href="https://github.com/elias-michaias/c-autotmpl/issues">Report an issue</a>
</p>
