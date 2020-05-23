#include <assert.h>
#include <ciggurat.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void test(CigSystemCtx *ctx, double dt) {
  int *i = cig_system_get_component(ctx, 1);
  *i += 1;
}

int main() {
  CigWorld *w = cig_world_init();
  assert(w != NULL);

  CigTypeDesc int_desc = {"int", sizeof(int), _Alignof(int)};
  CigTypeDesc float_desc = {"float", sizeof(float), _Alignof(float)};
  CigTypeDesc char_desc = {"char", sizeof(char), _Alignof(char)};
  CigTypeDesc short_desc = {"short", sizeof(short), _Alignof(short)};
  assert(!cig_world_register_type(w, &int_desc));
  assert(!cig_world_register_type(w, &float_desc));
  assert(!cig_world_register_type(w, &char_desc));
  assert(!cig_world_register_type(w, &short_desc));

  CigSystemDesc test_system_desc = {"test", "char, int", .func = test};
  CigSystemDesc test2_system_desc = {"test2", "float, int, short",
                                     .func = test};
  assert(!cig_world_register_system(w, &test_system_desc));
  assert(!cig_world_register_system(w, &test2_system_desc));

  {
    const CigEntity *e =
        cig_world_spawn(w, 10000000, "int, char, float, short");

    {
      float *f = cig_world_get_component(w, e[0], "float");
      assert(*f == 0.0f);
      *f = 123.0f;
    }

    {
      int *i = cig_world_get_component(w, e[1], "int");
      assert(*i == 0);
      *i = 65;
    }

    assert(*((float *)cig_world_get_component(w, e[0], "float")) == 123.0f);
    assert(*((int *)cig_world_get_component(w, e[1], "int")) == 65);

    assert(!cig_world_run(w, "test", 0));

    int *i = cig_world_get_component(w, e[1], "int");
    assert(*i == 66);
  }

  float t, n;
  t = (float)clock() / CLOCKS_PER_SEC;
  for (int i = 0; i < 5; i++) {
    assert(!cig_world_step(w, 0));
    n = (float)clock() / CLOCKS_PER_SEC;
    printf("World step took %f seconds.\n", n - t);
    t = n;
  }

  cig_world_deinit(w);
  return EXIT_SUCCESS;
}
