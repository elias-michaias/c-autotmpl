// clang-format off
#include <stdio.h>
#include "foo.h"
#include "gen/box.h"
#include "gen/list.h"

void list_test() {
  puts("LIST TEST:");
  List(int) l1 = {12};
  List(int) l2 = {24};
  List(int) l3 = {36};
  $(List.push, &l1, &l2);
  $(List.push, &l1, &l3);

  let final = $(List.pop, &l1);
  printf("%d\n", final->value);

  let last = $(List.last, &l1);
  let first = $(List.first, &l1);
  printf("%d\n", last->value);
  printf("%d\n", first->value);

  List(int) test = {99};
  $(List.set, &l1, 1, &test);
  let second = $(List.get, &l1, 1);
  printf("%d\n", second->value);

  List(Box(int)) lb = {0};
  List(Box(List(int))) lbb = {0};
  (void)lb;
  (void)lbb;
}

void box_test() {
  puts("BOX TEST:");
  Box(char) b1 = {12};
  Box(int) b2 = {19};
  let x = $(Container.get, &b1);
  $(Container.set, &b2, 34);
  let y = $(Container.get, &b2);
  printf("%d\n", x + y);
}

int main(void) {
  box_test();
  list_test();
  return 0;
}
