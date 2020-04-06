#ifndef CIG_H
#define CIG_H

#include <stddef.h>
#include <stdint.h>

typedef struct World World;
typedef uint64_t Entity;

typedef void (*SystemFn)(void *ctx, double dt);

typedef struct TypeDesc {
  char *identifier;
  size_t size, alignment;
} TypeDesc;

typedef struct SystemDesc {
  char *identifier;
  char *must_have_types;
  char *must_not_have_types;
  SystemFn fn;
} SystemDesc;

World *cig_world_init();
void cig_world_deinit(World *w);
int cig_world_register_type(World *w, TypeDesc *desc);
int cig_world_register_system(World *w, SystemDesc *desc);
const Entity *cig_world_spawn(World *w, size_t count, char *types);

#endif
