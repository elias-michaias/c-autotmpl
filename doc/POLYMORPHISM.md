# Polymorphism with template headers

A template header is included once per type. c-autotmpl writes the `#define` and
`#include` block, so one header can check its parameters, generate helpers,
contribute a generic-call case, or define a trait impl.

Most examples below are complete. Build the CLI from this repo, put it on your
`PATH`, and run the commands shown. The ad-hoc overloading example links to the
full fixture in the usage guide.

- [Checked constraints and generated functions](#checked-constraints-and-generated-functions)
- [Function templates](#function-templates)
- [Ad-hoc overloading and explicit method types](#ad-hoc-overloading-and-explicit-method-types)
- [c-trait impls](#c-trait-generic-impls)
- [Tagged unions with generated payloads](#tagged-unions-with-generated-payloads)
- [Extendible `_Generic` with CC.h](#extendible-generic-calls)

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

```sh
autotmpl --root . src/main.c
cc -std=c11 -I. src/main.c -o demo && ./demo
# right=(10,20) sum=33
# ints=10,14 floats=2.0,3.0
```

Two negative checks show the constraints. Run them last, because they add
`Pair(Big)` and `Ops(Point)` to the same generated headers:

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
autotmpl --root . src/bad_xy.c
cc -std=c11 -I. -fsyntax-only src/bad_xy.c
# error: static assertion failed: "Pair(T): T must fit in a pointer"

autotmpl --root . src/bad_ops.c
cc -std=c11 -I. -fsyntax-only src/bad_ops.c
# error: invalid operands to binary + (have 'Point' and 'Point')
```

Generated names are plain C names with `T` pasted in:

| Source name | `T = Point` | `T = int` |
| --- | --- | --- |
| `Pair(T)` | `Pair_Point` | `Pair_int` |
| `pair_set_right(T)` | `pair_set_right_Point` | `pair_set_right_int` |
| `pair_xy_sum(T)` | `pair_xy_sum_Point` | `pair_xy_sum_int` |
| `Ops(T)` / `ops_add(T)` | fails the operator check | `Ops_int` / `ops_add_int` |

The call site still names `T` in `pair_xy_sum(Point)` or `ops_add(int)`. These
functions are generic by generation, not by call-site selection.

## Function templates

Use a `function` decorator when the operation needs more than one type. A header
marked `// -- template function convert(From, To)` has parameter names, not
fixed types. A call site supplies them as `convert(float, int)(2.5f)`.
c-autotmpl writes one block with `#define From float`, `#define To int`, and
`#undef`s in reverse order. Helper names such as `convert_impl` are not usages,
and there is no AST deduction. [USAGE.md](USAGE.md#function-templates) has the
full header and generated-block rules.

## Ad-hoc overloading and explicit method types
With `dispatch=1` and `dispatch_truncate=1`, one public spelling can select
separately generated implementations. Concrete C names are pasted from template
arguments, so owners with the same argument list would collide. `// -- +owner`
(or `// -- mangle +owner`) appends the owning template name as the final paste
component. It is opt-in and independent of `explicit`. List the owners in
`dispatch_templates` so they contribute to the shared dispatch header.

For ordinary operations, the implementation gains no public parameter; the
generated block defines `Owner` while expanding the section. This abbreviated
`Box(T)` header uses `GET3` as its paste macro:

```c
/* tmpl/box.h */
// -- template type Box(T)
// -- +owner
#define get_impl(T, Owner) GET3(get, T, Owner)
static inline T get_impl(T, Owner)(Box(T) *b);
```

A `Container(float)` section using `// -- mangle +owner` has the same shape.
Both calls stay distinct even though each owner takes one argument:

```c
int x = get(&b);    /* get_int_Box */
float y = get(&c);  /* get_float_Container */
```

For a
[curried method with explicit type arguments](USAGE.md#curried-methods-with-explicit-type-arguments),
the implementation declaration gains a trailing synthetic `Owner` parameter:

```c
// -- template function option_map_impl(T, U, Owner)
// -- dispatch method Option.map explicit U
// -- +owner
#define option_map_impl(T, U, Owner) MAP4(map, T, U, Owner)
```

Callers still write `map(float)(o, f)`; the generator supplies `Owner`. The
concrete target is `<op>_<owner args>_<explicit args>_<Owner>`. The full
`Box`/`Container`/`Option`/`Result` fixture and generated dispatch tables are in
[Owner-qualified targets](USAGE.md#owner-qualified-targets).

<a id="c-trait-generic-impls"></a>

## c-trait impls from one header

[c-trait](https://github.com/elias-michaias/c-trait) uses `_Generic` to turn a
trait call into a direct call to the concrete implementation. Its associated
type pattern lets one trait keep a single method signature across generated
types. That matches a template header well: generate the struct and the trait
impl from the same `T`.

Copy `trait.h` from c-trait into the project root. No `dynamic(Self)` marker is
used, so dispatch is static and there is no vtable.

> **Compiler support:** c-trait's ISO C23 mode is fully standard and works with
> GCC, Clang, Intel C Compiler, and other conforming C23 compilers. Its GNU99
> and GNU11 modes use GNU extensions and work with GCC and Clang, plus compilers
> such as Intel C Compiler when they accept those GNU extensions. Use
> `-std=c23` for ISO C23 or `-std=gnu99` for GNU99. With a strict compiler that
> rejects `$` as an identifier, define `TraitCustomDispatch` and alias
> `trait_dispatch_call` to a plain name. See
> [c-trait compatibility](https://github.com/elias-michaias/c-trait#compatibility).

Use the guarded `point.h` from the first example.

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

A function template can call the generated trait overloads directly. Passing
the item type is enough: the function head spells out `List(Item)`, so
c-autotmpl lexes `List(int)` and `List(float)` out of the header and emits those
blocks first.

```c
/* tmpl/list_sum.h */
// -- template function list_sum(Item)

#include "../trait.h"

#ifndef Item
typedef int Item;
#endif

#define LIST_SUM_CAT_I(a, b) a##b
#define LIST_SUM_CAT(a, b) LIST_SUM_CAT_I(a, b)
#define list_sum(Item) LIST_SUM_CAT(list_sum_, Item)

static inline Item list_sum(Item)(List(Item) *xs) {
  return $(List.get, xs, 0) + $(List.get, xs, 1);
}

#undef Item
```

No dummy wrapper type or extra trait impl is needed just to reach the overload.
The dependency blocks are flattened into `gen/list_sum.h` ahead of the
functions, so no manual `GEN_LIST_H` guard is needed.

```c
/* src/main.c */
#include "point.h"
#include "gen/list.h"
#include "gen/list_sum.h"
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

  printf("xs=%d,%d fs=%.1f pt=(%d,%d) sums=%d,%.1f\n",
         $(List.get, &xs, 0), $(List.get, &xs, 1),
         $(List.get, &fs, 0),
         $(List.get, &ps, 0).x, $(List.get, &ps, 0).y,
         list_sum(int)(&xs),
         list_sum(float)(&fs));
  return 0;
}
```

`point.h` comes first because `List(Point)` needs `Point` and its `POINT_H`
guard. The function's own `List(int)` and `List(float)` dependencies are already
inside `gen/list_sum.h`.

```sh
cp /path/to/c-trait/trait.h .
autotmpl --root . src/main.c
cc -std=gnu11 -I. src/main.c -o demo && ./demo
# xs=7,9 fs=3.5 pt=(4,5) sums=16,3.5
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

`list_sum(int)` and `list_sum(float)` add free functions that call
`$(List.get, ...)` on the concrete list. The overload stays inside the generated
function, so the call site only names the item type.

See the [c-trait API reference](https://github.com/elias-michaias/c-trait/blob/main/docs/API.md)
for defaults, dynamic dispatch, and supertraits.

## Tagged unions with generated payloads

A template can generate the tag enum, payload struct, and constructors together.
Each instantiation gets its own tag constants and helpers, so `Maybe(int)` and
`Maybe(Point)` are distinct types with distinct operations.

```c
/* tmpl/maybe.h */
// -- template Maybe(T)

#ifndef T
typedef int T;
#endif

#define MAYBE_CAT_I(a, b) a##b
#define MAYBE_CAT(a, b) MAYBE_CAT_I(a, b)
#define Maybe(T) MAYBE_CAT(Maybe_, T)
#define Maybe_tag(T) MAYBE_CAT(Maybe_tag_, T)
#define maybe_some(T) MAYBE_CAT(maybe_some_, T)
#define maybe_none(T) MAYBE_CAT(maybe_none_, T)
#define maybe_has(T) MAYBE_CAT(maybe_has_, T)
#define maybe_value(T) MAYBE_CAT(maybe_value_, T)

typedef enum {
  MAYBE_CAT(Maybe_has_value_, T),
  MAYBE_CAT(Maybe_is_empty_, T)
} Maybe_tag(T);

typedef struct { Maybe_tag(T) tag; T value; } Maybe(T);

static inline Maybe(T) maybe_some(T)(T value) {
  Maybe(T) r = { MAYBE_CAT(Maybe_has_value_, T), value };
  return r;
}

static inline Maybe(T) maybe_none(T)(void) {
  Maybe(T) r = {0};
  r.tag = MAYBE_CAT(Maybe_is_empty_, T);
  return r;
}

static inline int maybe_has(T)(const Maybe(T) *o) {
  return o->tag == MAYBE_CAT(Maybe_has_value_, T);
}

static inline T maybe_value(T)(const Maybe(T) *o) {
  return o->value;
}

#undef T
```

Use the guarded `point.h` from the first example.

```c
/* src/main.c */
#include "point.h"
#include "gen/maybe.h"
#include <stdio.h>

int main(void) {
  Maybe(int) oi = maybe_some(int)(7);
  Maybe(Point) op = maybe_some(Point)((Point){2, 3});
  Maybe(int) empty = maybe_none(int)();

  printf("int=%d,%d point=%d,(%d,%d) empty=%d\n",
         maybe_has(int)(&oi), maybe_value(int)(&oi),
         maybe_has(Point)(&op), maybe_value(Point)(&op).x,
         maybe_value(Point)(&op).y, maybe_has(int)(&empty));
  return 0;
}
```

```sh
autotmpl --root . src/main.c
cc -std=c11 -I. src/main.c -o demo && ./demo
# int=1,7 point=1,(2,3) empty=0
```

This is a tagged union with type-mangled constructors. The payload type changes,
but the generated API shape stays the same.

<a id="extendible-generic-calls"></a>

## Extendible `_Generic` with CC.h

C11 `_Generic` gives static dispatch, but its association list is fixed where
the macro is defined. Jackson Allan's CC.h article describes a way to leave
empty slots in that list and let users fill them with type and function pairs.
The technique is the basis for CC's comparison, hash, and destructor dispatch.

- [Better C Generics Part 1: The Extendible `_Generic`](https://github.com/JacksonAllan/CC/blob/main/articles/Better_C_Generics_Part_1_The_Extendible_Generic.md)
- [Convenient Containers](https://github.com/JacksonAllan/CC)

The shorter slotted form below uses three registration slots. A template header
turns each `Register(T)` usage into one registration, so adding a type is the
same edit as adding a container instantiation.

> **Standard dependency:** This approach needs C11 or later because `_Generic`
> is a C11 keyword. The example uses `-std=gnu11`; `-std=c11` also works. There
> is no C99 version of this exact macro. CC and the article's counter variant
> build on the same C11 feature.

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

---

<p align="center">
  <a href="../README.md">Home</a> &bull;
  <a href="USAGE.md">Usage</a> &bull;
  <a href="https://github.com/elias-michaias/c-autotmpl/issues">Report an issue</a>
</p>
