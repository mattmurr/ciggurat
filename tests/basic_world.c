#include <assert.h>
#include <ciggurat.h>
#include <stdio.h>
#include <stdlib.h>

void test(void *ctx, double dt) {}

int main() {
  World *w = cig_world_init();
  assert(w != NULL);

  TypeDesc int_desc = {"int", sizeof(int), _Alignof(int)};
  TypeDesc float_desc = {"float", sizeof(float), _Alignof(float)};
  TypeDesc char_desc = {"char", sizeof(char), _Alignof(char)};
  TypeDesc short_desc = {"short", sizeof(short), _Alignof(short)};
  assert(!cig_world_register_type(w, &int_desc));
  assert(!cig_world_register_type(w, &float_desc));
  assert(!cig_world_register_type(w, &char_desc));
  assert(!cig_world_register_type(w, &short_desc));

  SystemDesc test_system_desc = {"test", "char, int", .fn = test};
  SystemDesc test2_system_desc = {"test2", "int, float, short", .fn = test};
  assert(!cig_world_register_system(w, &test_system_desc));
  assert(!cig_world_register_system(w, &test2_system_desc));

  const Entity *e = cig_world_spawn(w, 10000, "int, char, float, short");

  float *f = cig_get_component(w, e[0], "float");
  assert(*f == 0.0f);
  *f = 123.0f;

  int *i = cig_get_component(w, e[1], "int");
  assert(*i == 0);
  *i = 65;

  assert(*((float *)cig_get_component(w, e[0], "float")) == 123.0f);
  assert(*((int *)cig_get_component(w, e[1], "int")) == 65);

  assert(!cig_world_run(w, "test", 0));
  assert(!cig_world_step(w, 0));

  cig_world_deinit(w);
  return EXIT_SUCCESS;
}
