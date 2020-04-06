#include <ciggurat.h>
#include <mylib/mylib.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK_KB_SIZE 16
#define CHUNK_BYTE_SIZE CHUNK_KB_SIZE * 1024

struct entity {
  // The storage that contains this entity's types. The storage also contains
  // the mask for the entity.
  struct storage *storage;
  // A pointer to the entity's types in the storage.
  void *owned_types;
};

struct layout {
  // Keeps track of the type id and corresponding size which may or may not
  // contain padding.
  struct types {
    size_t id;
    size_t size;
  } * types;

  // The total size of a single family when packed.
  size_t family_size;

  // The alignment for the family, derived from the widest type.
  size_t alignment;
};

struct storage {
  // The mask generated for the combination of types that this storage contains.
  Bitset mask;
  struct layout layout;

  // Contains `struct region`
  LinkedList chunks;

  // Contains `void *`
  Vector recycled;

  // Contains systems that have matched with this storage.
  HashMap systems;
};

struct system {
  // An string identifier/name for the system used for the hash.
  char *identifier;

  // An array of type ids that this system operates on so we know the order in
  // which the types were defined.
  size_t *types;

  // Requirements for the system to match with a storage/entity.
  Bitset must_have, must_not_have;

  // Contains storages that have matched with this system.
  HashMap storages;

  SystemFn fn;
};

typedef struct World {
  // Contains `TypeDesc`.
  Vector types;
  // Holds the storage for each used combination of types.
  HashMap storages;
  // Holds all of the registered systems.
  Vector systems;

  // Keep track of the next Entity ID to use.
  Entity next_entity;
  // Contains `struct entity`.
  Vector entities;
  // Contains `Entity`
  Vector recycled;
  // Runtime allocated array of the last entities that were spawned.
  Entity *last_spawned;
} World;

static int storage_init(struct storage *result) { return EXIT_SUCCESS; }

static void storage_deinit(struct storage *storage) {}

static int system_init(struct system *result, SystemDesc *desc) {
  return EXIT_SUCCESS;
}

static void system_deinit(struct system *system) {}

World *cig_world_init() {
  World *result = calloc(1, sizeof(World));
  if (!result)
    return result;

  if (hash_map_init(&result->storages, bitset_hash, bitset_eql, sizeof(Bitset),
                    sizeof(struct storage)))
    goto err;

  if (vector_init(&result->systems, sizeof(struct system)))
    goto err;

  if (vector_init(&result->entities, sizeof(struct entity)))
    goto err;

  if (vector_init(&result->recycled, sizeof(Entity)))
    goto err;

  return result;

err:
  vector_deinit(&result->recycled);
  vector_deinit(&result->entities);
  vector_deinit(&result->systems);
  hash_map_deinit(&result->storages);
  free(result);

  return NULL;
}

void cig_world_deinit(World *w) {
  if (w == NULL)
    return;

  HashMapIterator it = hash_map_iter(&w->storages);
  const HashMapKV *next;
  while ((next = hash_map_next(&it)))
    storage_deinit((struct storage *)next->value);
  hash_map_deinit(&w->storages);

  struct system *systems = w->systems.data;
  for (size_t i = 0; i < vector_len(&w->systems); i++)
    system_deinit(&systems[i]);
  vector_deinit(&w->systems);

  vector_deinit(&w->entities);
  vector_deinit(&w->recycled);
  free(w->last_spawned);
}

int cig_world_register_type(World *w, TypeDesc *desc) {
  assert(w != NULL);
  assert(desc != NULL);

  TypeDesc *types = w->types.data;
  for (size_t i = 0; i < vector_len(&w->types); i++)
    if (strcmp(types[i].identifier, desc->identifier)) {
#ifdef DEBUG
      fprintf(stderr, "%s(): Type with identifier already registered (%s).",
              __func__, desc->identifier);
#endif
      return EXIT_FAILURE;
    }

  if (vector_append(&w->types, desc))
    return EXIT_FAILURE;

#ifdef DEBUG
  printf("%s(): Type registered (%s).", __func__, desc->identifier);
#endif

  return EXIT_SUCCESS;
}

static char **tokenize(char *str, size_t *size) {
  // TODO Remove whitespace.
  // TODO Split the string up into multiple parts using the comma delimiter.
}

int cig_world_register_system(World *w, SystemDesc *desc) {
  SystemDesc *systems = w->systems.data;
  for (size_t i = 0; i < vector_len(&w->systems); i++)
    if (strcmp(systems[i].identifier, desc->identifier)) {
#ifdef DEBUG
      fprintf(stderr, "%s(): System with identifier already registered(%s).",
              __func__, desc->identifier);
#endif
      return EXIT_FAILURE;
    }

  // Check the system type requirements are satisfied.
  // Tokenize the desc->must_have string.
  size_t size;
  char **tokens = tokenize(desc->must_have_types, &size);
  if (!tokens)
    return EXIT_FAILURE;

  // Check that there is at least 1 type identifier in `must_have`.
  if (size == 0) {
#ifdef DEBUG
    fprintf(stderr,
            "%s(): System `must_have` requires at least 1 type identifier.\n",
            __func__);
#endif
    return EXIT_FAILURE;
  }

  // Ensure each type exists in the world.
  int result = 1;
  TypeDesc *types = w->types.data;
  for (size_t i = 0; i < size; i++) {
    for (size_t j = 0; j < vector_len(&w->types); j++) {
      if (strcmp(tokens[i], types[j].identifier))
        goto next_token;
    }
    result = 0;
#ifdef DEBUG
    fprintf(
        stderr,
        "%s(): Required type (identifier: %s) does not exist in the world.\n",
        __func__, tokens[i]);
#endif
  next_token:;
  }
  if (!result)
    return EXIT_FAILURE;

  struct system system;
  if (system_init(&system, desc))
    return EXIT_FAILURE;

  if (vector_append(&w->systems, &system)) {
    system_deinit(&system);
    return EXIT_FAILURE;
  }

#ifdef DEBUG
  printf("%s(): System registered (%s).", __func__, desc->identifier);
#endif

  return EXIT_SUCCESS;
}
