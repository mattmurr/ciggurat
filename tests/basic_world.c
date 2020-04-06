#include <assert.h>
#include <ciggurat.h>
#include <stdio.h>
#include <stdlib.h>

void test(void *ctx, double dt) { printf("test called.\n"); }

int main() {
  World *w = cig_world_init();
  assert(w != NULL);

  TypeDesc int_desc = {"int", sizeof(int), _Alignof(int)};
  assert(!cig_world_register_type(w, &int_desc));

  SystemDesc test_system_desc = {"test", "int", test};
  assert(!cig_world_register_system(w, &test_system_desc));

  cig_world_deinit(w);
  return EXIT_SUCCESS;
}
