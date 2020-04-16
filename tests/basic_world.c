#include <assert.h>
#include <ciggurat.h>
#include <stdio.h>
#include <stdlib.h>

void test(void *ctx, double dt) { printf("test called.\n"); }

int main() {
  World *w = cig_world_init();
  assert(w != NULL);

  TypeDesc int_desc = {"int", sizeof(int), _Alignof(int)};
  TypeDesc float_desc = {"float", sizeof(float), _Alignof(float)};
  assert(!cig_world_register_type(w, &int_desc));
  assert(!cig_world_register_type(w, &float_desc));

  SystemDesc test_system_desc = {"test", "int, float", .fn = test};
  assert(!cig_world_register_system(w, &test_system_desc));

  cig_world_deinit(w);
  return EXIT_SUCCESS;
}
