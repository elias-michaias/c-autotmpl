// clang-format off
#ifndef CONTAINER_H
#define CONTAINER_H

#include <trait.h>
#define Trait Container
#define ContainerSignature(Self) \
  required(Self, Container_InnerType, get) \
  required(Self, void, set, Container_InnerType)
#include <trait.h>

#endif
