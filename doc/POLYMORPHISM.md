<p align="center">
  <h1 align="center">Polymorphism with template headers</h1>
  <p align="center">Checked constraints, generated functions, dispatch, and trait impls from one C header.</p>
</p>

<p align="center">
  <a href="../LICENSE.md"><img src="https://img.shields.io/badge/license-MIT-blue" alt="MIT license"></a>
  <a href="https://github.com/elias-michaias/c-autotmpl/actions/workflows/cli-build.yml"><img src="https://github.com/elias-michaias/c-autotmpl/actions/workflows/cli-build.yml/badge.svg" alt="CI status"></a>
</p>

<p align="center">
  <a href="../README.md">Home</a> &bull;
  <a href="USAGE.md">Usage</a> &bull;
  <a href="BUILD.md">Build</a> &bull;
  <a href="#checked-constraints-and-generated-functions">Constraints</a> &bull;
  <a href="#extendible-generic-dispatch">Dispatch</a> &bull;
  <a href="#c-trait-generic-impls">Traits</a>
</p>

---

A template header is included once per type. c-autotmpl writes the
`#define T` and `#include` block, so the header can generate more than a type.
It can check `T`, write helper functions that take `T`, or register a dispatch
entry and trait impl for that type.

Every example below is complete. Build the CLI from this repo, put it on your
`PATH`, then run the commands shown with each example.

- [Checked constraints and generated functions](#checked-constraints-and-generated-functions)
- [Extendible generic dispatch](#extendible-generic-dispatch)
- [c-trait generic impls](#c-trait-generic-impls)
- [Tagged unions with generated payloads](#tagged-unions-with-generated-payloads)
- [Function-pointer dispatch](#function-pointer-dispatch)
- [Intrusive containers with checked link fields](#intrusive-containers-with-checked-link-fields)

## Checked constraints and generated functions

A template header should still parse on its own in an editor. The `#ifndef T`
fallback gives tools a concrete type. Generated code defines `T` before the
include, so the fallback never reaches a real instantiation.

`Pair(T)` stores two `T` values and generates two functions beside the type.
The checks require `T` to fit in a pointer and to expose `x` and `y` members.
`pair_set_right` takes a `T` and assigns it to the right side of the pair.
`pair_xy_sum` reads `x` and `y` from any `T` that satisfies the checks.

Use a lowercase prefix for generated helper macros. Anything that starts with
the template name and `_`, such as `Pair_set_right`, can be read by the scanner
as a mangled type and produce a bogus `Pair(set_right)` block.

```c
/* tmpl/pair.h */
// -- template Pair(T)

#ifndef T
typedef struct { int x; int y; } T;
#endif

_Static_assert(sizeof(T) <= sizeof(void *), "Pair(T): T must fit in a pointer");
_Static_assert(_Alignof(T) <= _Alignof(void *), "Pair(T): T must not be over-aligned");
_Static_assert(sizeof(((T *)0)->x) * sizeof(((T *)0)->y) != 0,
               "Pair(T): T must have x and y members");

#define PAIR_CAT_I(a, b) a##b
#define PAIR_CAT(a, b) PAIR_CAT_I(a, b)
#define Pair(T) PAIR_CAT(Pair_, T)
#define pair_set_right(T) PAIR_CAT(pair_set_right_, T)
#define pair_xy_sum(T) PAIR_CAT(pair_xy_sum_, T)

typedef struct { T first; T second; } Pair(T);

static inline void pair_set_right(T)(Pair(T) *p, T value) {
  p->second = value;
}

static inline int pair_xy_sum(T)(const Pair(T) *p) {
  return p->first.x + p->first.y + p->second.x + p->second.y;
}

#undef T
```

The binary operator checks are plain expressions. `Ops(T)` generates ordinary
functions that use `+` and `*`. Instantiating with `int` or `float` just works.
A type without those operators fails at the assertion or at the operator use.

```c
/* tmpl/ops.h */
// -- template Ops(T)

#ifndef T
typedef int T;
#endif

_Static_assert(sizeof((T)0 + (T)0) > 0, "Ops(T): T must support +");
_Static_assert(sizeof((T)0 * (T)0) > 0, "Ops(T): T must support *");

#define OPS_CAT_I(a, b) a##b
#define OPS_CAT(a, b) OPS_CAT_I(a, b)
#define Ops(T) OPS_CAT(Ops_, T)
#define ops_add(T) OPS_CAT(ops_add_, T)
#define ops_twice(T) OPS_CAT(ops_twice_, T)

typedef struct { T value; } Ops(T);

static inline T ops_add(T)(T a, T b) { return a + b; }
static inline T ops_twice(T)(T v) { return v * 2; }

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
#include "gen/ops.h"
#include <stdio.h>

int main(void) {
  Pair(Point) p = { {1, 2}, {3, 4} };
  Point new_right = { 10, 20 };

  pair_set_right(Point)(&p, new_right);

  Ops(int) oi = { 7 };
  Ops(float) of = { 1.5f };

  printf("right=(%d,%d) sum=%d\n",
         p.second.x, p.second.y, pair_xy_sum(Point)(&p));
  printf("ints=%d,%d floats=%.1f,%.1f\n",
         ops_add(int)(oi.value, 3), ops_twice(int)(oi.value),
         ops_add(float)(of.value, 0.5f), ops_twice(float)(of.value));
  return 0;
}
```

```c
/* src/bad_xy.c */
#include "big.h"
#include "gen/pair.h"

Pair(Big) bad = {0};

int main(void) { return (int)sizeof(bad); }
```

```c
/* src/bad_ops.c */
#include "point.h"
#include "gen/ops.h"

Ops(Point) bad = {0};

int main(void) { return (int)ops_add(Point)(bad.value, bad.value); }
```

```sh
autotmpl --root . src/main.c
cc -std=c11 -I. src/main.c -o demo && ./demo
# right=(10,20) sum=33
# ints=10,14 floats=2.0,3.0

autotmpl --root . src/bad_xy.c
cc -std=c11 -I. -fsyntax-only src/bad_xy.c
# error: static assertion failed: "Pair(T): T must fit in a pointer"

autotmpl --root . src/bad_ops.c
cc -std=c11 -I. -fsyntax-only src/bad_ops.c
# error: invalid operands to binary + (have 'Point' and 'Point')
```

Run the two failing checks last. They add `Pair(Big)` and `Ops(Point)` to the
same generated headers.

The generated names are plain C names with `T` pasted in:

| Source name | `T = Point` | `T = int` |
| --- | --- | --- |
| `Pair(T)` | `Pair_Point` | not used here |
| `pair_set_right(T)` | `pair_set_right_Point` | not used here |
| `pair_xy_sum(T)` | `pair_xy_sum_Point` | not used here |
| `Ops(T)` | fails the operator check | `Ops_int` |
| `ops_add(T)` | fails the operator check | `ops_add_int` |

The call site still names `T` in `pair_xy_sum(Point)` or `ops_add(int)`. These
are generic functions by generation, not by dispatch. When the caller should
not care which concrete function it calls, the next step is dispatch. The
following section uses extendible `_Generic`. The one after that uses c-trait
to generate the impl and the dispatcher together.

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

## Tagged unions with generated payloads

A template can generate the tag enum, payload struct, and constructors together.
Each instantiation gets its own tag constants and helpers, so `Option(int)` and
`Option(Point)` are distinct types with distinct operations.

```c
/* tmpl/option.h */
// -- template Option(T)

#ifndef T
typedef int T;
#endif

#define OPTION_CAT_I(a, b) a##b
#define OPTION_CAT(a, b) OPTION_CAT_I(a, b)
#define Option(T) OPTION_CAT(Option_, T)
#define Option_tag(T) OPTION_CAT(Option_tag_, T)
#define option_some(T) OPTION_CAT(option_some_, T)
#define option_none(T) OPTION_CAT(option_none_, T)
#define option_has(T) OPTION_CAT(option_has_, T)
#define option_value(T) OPTION_CAT(option_value_, T)

typedef enum {
  OPTION_CAT(Option_has_value_, T),
  OPTION_CAT(Option_is_empty_, T)
} Option_tag(T);

typedef struct { Option_tag(T) tag; T value; } Option(T);

static inline Option(T) option_some(T)(T value) {
  Option(T) r = { OPTION_CAT(Option_has_value_, T), value };
  return r;
}

static inline Option(T) option_none(T)(void) {
  Option(T) r = {0};
  r.tag = OPTION_CAT(Option_is_empty_, T);
  return r;
}

static inline int option_has(T)(const Option(T) *o) {
  return o->tag == OPTION_CAT(Option_has_value_, T);
}

static inline T option_value(T)(const Option(T) *o) {
  return o->value;
}

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
/* src/main.c */
#include "point.h"
#include "gen/option.h"
#include <stdio.h>

int main(void) {
  Option(int) oi = option_some(int)(7);
  Option(Point) op = option_some(Point)((Point){2, 3});
  Option(int) empty = option_none(int)();

  printf("int=%d,%d point=%d,(%d,%d) empty=%d\n",
         option_has(int)(&oi), option_value(int)(&oi),
         option_has(Point)(&op), option_value(Point)(&op).x,
         option_value(Point)(&op).y, option_has(int)(&empty));
  return 0;
}
```

```sh
autotmpl --root . src/main.c
cc -std=c11 -I. src/main.c -o demo && ./demo
# int=1,7 point=1,(2,3) empty=0
```

This is a sum type with type-mangled constructors. The payload type changes,
but the generated API shape stays the same.

## Function-pointer dispatch

C does not need a trait library for runtime polymorphism. A template can
generate a small handle that stores a `self` pointer and a set of operations.
Callers pass different operation sets and call through the handle.

```c
/* tmpl/draw.h */
// -- template Draw(T)

#ifndef T
typedef int T;
#endif

#define DRAW_CAT_I(a, b) a##b
#define DRAW_CAT(a, b) DRAW_CAT_I(a, b)
#define Draw(T) DRAW_CAT(Draw_, T)
#define draw_make(T) DRAW_CAT(draw_make_, T)
#define draw_area(T) DRAW_CAT(draw_area_, T)
#define draw_describe(T) DRAW_CAT(draw_describe_, T)

typedef struct {
  const void *self;
  double (*area)(const void *);
  void (*describe)(const void *);
} Draw(T);

static inline Draw(T) draw_make(T)(const T *self,
                                   double (*area)(const void *),
                                   void (*describe)(const void *)) {
  Draw(T) d = { self, area, describe };
  return d;
}

static inline double draw_area(T)(const Draw(T) *d) {
  return d->area(d->self);
}

static inline void draw_describe(T)(const Draw(T) *d) {
  d->describe(d->self);
}

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
/* rect.h */
#ifndef RECT_H
#define RECT_H

typedef struct { double w; double h; } Rect;

#endif
```

```c
/* src/main.c */
#include "point.h"
#include "rect.h"
#include "gen/draw.h"
#include <stdio.h>

static double point_area(const void *p) {
  (void)p;
  return 0.0;
}

static void point_describe(const void *p) {
  const Point *q = (const Point *)p;
  printf("Point(%d,%d)\n", q->x, q->y);
}

static double rect_area(const void *p) {
  const Rect *r = (const Rect *)p;
  return r->w * r->h;
}

static void rect_describe(const void *p) {
  const Rect *r = (const Rect *)p;
  printf("Rect(%.1f,%.1f)\n", r->w, r->h);
}

int main(void) {
  Point p = { 3, 4 };
  Rect r = { 2.0, 3.5 };

  Draw(Point) dp = draw_make(Point)(&p, point_area, point_describe);
  Draw(Rect) dr = draw_make(Rect)(&r, rect_area, rect_describe);

  draw_describe(Point)(&dp);
  draw_describe(Rect)(&dr);
  printf("areas=%.1f,%.1f\n", draw_area(Point)(&dp), draw_area(Rect)(&dr));
  return 0;
}
```

```sh
autotmpl --root . src/main.c
cc -std=c11 -I. src/main.c -o demo && ./demo
# Point(3,4)
# Rect(2.0,3.5)
# areas=0.0,7.0
```

The handle is typed by `T`, while the operation signatures are uniform. That
keeps the call code simple and the operation tables explicit.

## Intrusive containers with checked link fields

An intrusive container stores link nodes inside the elements instead of
allocating separate nodes. The element type must provide the link field. A
template can check that field and generate typed push and pop functions around
the shared link code. See
[Intrusive Linked Lists](https://www.data-structures-in-practice.com/intrusive-linked-lists/)
for the underlying design.

```c
/* link.h */
#ifndef LINK_H
#define LINK_H

typedef struct Link {
  struct Link *next;
} Link;

#endif
```

```c
/* tmpl/intrusive.h */
// -- template Intrusive(T)

#include <stddef.h>
#include "../link.h"

#ifndef T
typedef struct { Link link; } T;
#endif

_Static_assert(sizeof(((T *)0)->link) == sizeof(Link),
               "Intrusive(T): T must embed a Link named link");

#define INTRUSIVE_CAT_I(a, b) a##b
#define INTRUSIVE_CAT(a, b) INTRUSIVE_CAT_I(a, b)
#define Intrusive(T) INTRUSIVE_CAT(Intrusive_, T)
#define intrusive_push(T) INTRUSIVE_CAT(intrusive_push_, T)
#define intrusive_pop(T) INTRUSIVE_CAT(intrusive_pop_, T)

typedef struct { Link *head; } Intrusive(T);

static inline void intrusive_push(T)(Intrusive(T) *list, T *item) {
  item->link.next = list->head;
  list->head = &item->link;
}

static inline T *intrusive_pop(T)(Intrusive(T) *list) {
  Link *node = list->head;
  if (!node) return NULL;
  list->head = node->next;
  return (T *)((char *)node - offsetof(T, link));
}

#undef T
```

```c
/* node.h */
#ifndef NODE_H
#define NODE_H

#include "link.h"

typedef struct Node {
  int value;
  Link link;
} Node;

#endif
```

```c
/* src/main.c */
#include "node.h"
#include "gen/intrusive.h"
#include <stdio.h>

int main(void) {
  Node a = { 1, {0} };
  Node b = { 2, {0} };
  Node c = { 3, {0} };
  Intrusive(Node) list = {0};

  intrusive_push(Node)(&list, &a);
  intrusive_push(Node)(&list, &b);
  intrusive_push(Node)(&list, &c);

  printf("%d %d %d\n",
         intrusive_pop(Node)(&list)->value,
         intrusive_pop(Node)(&list)->value,
         intrusive_pop(Node)(&list)->value);
  return 0;
}
```

```sh
autotmpl --root . src/main.c
cc -std=c11 -I. src/main.c -o demo && ./demo
# 3 2 1
```

The generated list type is just a link head. The `offsetof` cast turns a link
back into the concrete element type, so one template serves every element that
passes the link check.

---

<p align="center">
  <a href="../README.md">Home</a> &bull;
  <a href="USAGE.md">Usage</a> &bull;
  <a href="https://github.com/elias-michaias/c-autotmpl/issues">Report an issue</a>
</p>
