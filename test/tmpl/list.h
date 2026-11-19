// clang-format off
// -- template List(T)
#define List(T) List_IMPL(T)
#define List_IMPL(T) List_##T

#ifndef T 
typedef int T;
#endif

#include <stddef.h>
#include <prelude.h>
#include <stdint.h>
#include <trait.h>

#ifndef ListSignature
#define Trait List
#define ListSignature(Self) \
  required(Self, ListType, last) \
  required(Self, ListType, first) \
  required(Self, void, push, ListType) \
  required(Self, ListType, pop) \
  required(Self, ListType, get, uint16_t) \
  required(Self, void, set, uint16_t, ListType)
#include <trait.h>
#endif

typedef struct TYPE(List) TYPE(List);
struct TYPE(List) {
  T value;
  TYPE(List)* next; 
  TYPE(List)* prev;
};

#define Impl List
#define For TYPE(List)
#define ListType TYPE(List)*
static inline TYPE(List)* def(last) {
  var out = self;
  while (out->next) {
    out = out->next;
  }
  return out;
}

static inline TYPE(List)* def(first) {
  var out = self;
  while (out->prev) {
    out = out->prev;
  }
  return out;
}

static inline void def(push, TYPE(List)* r) {
  var last = self;
  while (last->next) {
    last = last->next;
  }
  last->next = r;
  r->prev = last;
}

static inline TYPE(List)* def(pop) {
  var last = self;
  while (last->next) {
    last = last->next;
  }

  if (last->prev)
    last->prev->next = NULL;

  last->prev = NULL;
  return last;
}

static inline TYPE(List)* def(get, uint16_t idx) {
  var out = self;
  while (out->prev) {
    out = out->prev;
  }

  while (idx-- && out)
    out = out->next;

  return out;
}

static inline void def(set, uint16_t idx, TYPE(List)* r) {
  var tmp = self;
  while (tmp->prev) {
    tmp = tmp->prev;
  }

  while (idx-- && tmp) tmp = tmp->next;
  if (!tmp || !r) return;

  r->prev = tmp->prev;
  r->next = tmp->next;

  if (tmp->prev)
    tmp->prev->next = r;

  if (tmp->next)
    tmp->next->prev = r;

  tmp->prev = NULL;
  tmp->next = NULL;
}

#include <trait.h>
