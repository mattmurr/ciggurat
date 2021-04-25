#ifndef CIG_H
#define CIG_H

#include <stddef.h>
#include <stdint.h>

typedef struct CigWorld CigWorld;
typedef uint64_t CigEntity;
typedef struct CigSystemCtx CigSystemCtx;

typedef void (*CigSystemFunc)(CigSystemCtx *ctx, double dt);

typedef struct CigTypeDesc {
  char *identifier;
  size_t size, alignment;
} CigTypeDesc;

typedef struct CigSystemDesc {
  char *identifier;
  char *requirements;
  CigSystemFunc func;
  void *user_data;
} CigSystemDesc;

void cig_world_deinit(CigWorld *w);
CigWorld *cig_world_init();
int cig_world_register_type(CigWorld *w, CigTypeDesc *desc);
int cig_world_register_system(CigWorld *w, CigSystemDesc *desc);
const CigEntity *cig_world_spawn(CigWorld *w, size_t count, const char *types);
void *cig_world_get_component(const CigWorld *w, const CigEntity e,
                              const char *type_str);
int cig_world_run(const CigWorld *w, const char *identifier, double delta_time);
int cig_world_step(const CigWorld *w, double delta_time);

void *cig_system_get_component(const CigSystemCtx *ctx, size_t idx);
void *cig_system_get_user_data(const CigSystemCtx *ctx);

#endif
