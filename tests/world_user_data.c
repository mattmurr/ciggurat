#include <assert.h>
#include <ciggurat.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void test(CigSystemCtx *ctx, double dt) {
  int *i = cig_system_get_component(ctx, 0);
  *i += 1;

  int *j = cig_system_get_user_data(ctx);
  *j = 50;
}

int main() {
  int user_data = 0;

  CigWorld *w = cig_world_init();
  assert(w != NULL);

  CigTypeDesc int_desc = {"int", sizeof(int), _Alignof(int)};
  assert(!cig_world_register_type(w, &int_desc));

  CigSystemDesc test_system_desc = {"test", "int", .func = test,
                                    .user_data = &user_data};
  assert(!cig_world_register_system(w, &test_system_desc));

  {
    cig_world_spawn(w, 1, "int");

    assert(!cig_world_run(w, "test", 0));
    assert(user_data == 50);
  }

  cig_world_deinit(w);
  return EXIT_SUCCESS;
}
