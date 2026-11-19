// clang-format off

// -- template type Box(T)
#define Box(T) Box_IMPL(T)
#define Box_IMPL(T) Box_##T

#include <prelude.h>
#include "../trait/container.h"
#include <trait.h>
#ifndef T
typedef int T;
#endif

typedef struct {
  T value;
} TYPE(Box);

#define Impl Container
#define For TYPE(Box)
#define Container_InnerType T

static inline T def(get) {
  return self->value;
}

static inline void def(set, T t) {
  self->value = t;
}

#include <trait.h>
#undef Container_InnerType
