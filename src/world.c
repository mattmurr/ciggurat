#define _POSIX_C_SOURCE 200112L
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
  int32_t *types;

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

static void system_deinit(struct system *system) {
  bitset_deinit(&system->must_not_have);
  bitset_deinit(&system->must_have);

  free(system->types);

  free(system->identifier);
}

// Splits a comma-seperated string of types into an array of token strings.
// Must also provide a size_t pointer for the size of the array returned.
static char **tokenize(const char *str, size_t *size) {
  char **result = NULL;

  // Duplicate types_str and remove any spaces from the str first.
  char *without_whitespace = strdup(str);
  if (!without_whitespace)
    return 0;

  size_t i, j;
  for (i = j = 0; i < strlen(without_whitespace); i++) {
    if (str[i] != ' ')
      without_whitespace[j++] = str[i];
  }
  for (; j <= i; j++)
    without_whitespace[j] = 0;

  // We want to split the str at any instance of the chars '!' and ','
  const char *delimiters = ",";

  // Keep track of how many entries are in the result array.
  *size = 0;

  // Split into an array of chars.
  char *token = strtok(without_whitespace, delimiters);

  while (token != NULL) {
    // Attempt to realloc the result
    char **new_arr = realloc(result, sizeof(char *) * (*size + 1));
    if (!new_arr)
      goto err;

    // Assign
    result = new_arr;

    // Duplicate the token into the result.
    result[*size] = strdup(token);
    if (!result[*size])
      goto err;
    (*size)++;

    // Attempt to get the next token.
    token = strtok(NULL, delimiters);
  }

  free(token);
  free(without_whitespace);

  return result;

err:
  free(token);
  free(without_whitespace);

  // Failed allocation, we need to free everything.
  if (result) {
    for (size_t i = 0; i < *size; i++)
      free(result[i]);
    free(result);
  }

  *size = 0;
  return NULL; // Return zero, indicating an empty result.
}

static int generate_system_masks(Bitset *masks, const char *type,
                                 const char *token, int32_t id, void *e) {
  // We passed a pointer to the system's `types` field
  int32_t **types = e;

  // `masks`[0] is must_have
  // `masks`[1] is must_not_have

  // Check the first character in the token
  switch (token[0]) {

  // Does it begin with an exclamation mark
  case '!':
    if (strcmp(&token[1], type) == 0) {
      bitset_incl(&masks[1], id);
      *(*types)++ = id;

      return EXIT_SUCCESS;
    }
    break;

  default:
    if (strcmp(token, type) == 0) {
      bitset_incl(&masks[0], id);
      *(*types)++ = id;

      return EXIT_SUCCESS;
    }
    break;
  }

  return EXIT_FAILURE;
}

static int populate_mask(World *w, Bitset *mask,
                         int (*f)(Bitset *, const char *, const char *, int32_t,
                                  void *),
                         const char *types_str, void *e) {
  // If tokens are not already initialized then we will tokenize and return.
  size_t size = 0;
  char **tokens = tokenize(types_str, &size);
  if (!tokens)
    return EXIT_FAILURE;

  int result = EXIT_SUCCESS;

  // `size` cannot be more than the count of registered types
  if (size > vector_len(&w->types)) {

    result = EXIT_FAILURE;
#ifdef DEBUG
    fprintf(stderr,
            "%s(): There are more types requested than types registered in the "
            "world. [%s]\n.",
            __func__, types_str);
#endif

  } else {
    TypeDesc *types = w->types.data;
    for (size_t i = 0; i < size; i++) {
      for (size_t j = 0; j < vector_len(&w->types); j++)
        // Call the `f` function pointer to generate the mask/s
        if (!f(mask, types[j].identifier, tokens[i], j, e))
          goto next;

      result = EXIT_FAILURE;
#ifdef DEBUG
      fprintf(
          stderr,
          "%s(): SystemDesc `requirements` required type (%s) does not exist "
          "in the world.\n",
          __func__, tokens[i]);
#endif

    next:;
    }
  }

  // Ensure that we free the tokens
  for (size_t i = 0; i < size; i++)
    free(tokens[i]);
  free(tokens);

  return result;
}

static int system_init(World *w, struct system *result, SystemDesc *desc) {
  *result = (struct system){0};

  // Loop through all existing systems to ensure that there is not already a
  // system registered with the same identifier
  SystemDesc *systems = w->systems.data;
  for (size_t i = 0; i < vector_len(&w->systems); i++)
    if (strcmp(systems[i].identifier, desc->identifier) == 0) {

#ifdef DEBUG
      fprintf(stderr, "%s(): System with identifier already registered(%s).\n",
              __func__, desc->identifier);
#endif
      return EXIT_FAILURE;
    }

  result->identifier = strdup(desc->identifier);
  if (!result->identifier)
    return EXIT_FAILURE;

  size_t registered_type_count = vector_len(&w->types);

  // TODO Instead of allocating the types array directly, pass a Vector into the
  // `populate_mask()` function.
  // TODO Implement Vector to array and trim helpers in mylib.

  // Allocate memory for the `types` array.
  size_t capacity = registered_type_count * sizeof(int32_t);
  result->types = malloc(capacity);
  if (!result->types)
    goto err;
  // Set the values to -1.
  memset(result->types, -1, capacity);

  // Initialize the masks.
  if (bitset_init(&result->must_have, registered_type_count) ||
      bitset_init(&result->must_not_have, registered_type_count))
    goto err;

  // Create an array with both masks to pass into `populate_mask()`
  Bitset masks[2] = {result->must_have, result->must_not_have};
  // and a copy of the pointer for the types array.
  int32_t *types = result->types;

  if (populate_mask(w, masks, generate_system_masks, desc->requirements,
                    &types))
    goto err;

  return EXIT_SUCCESS;

err:
  system_deinit(result);

  return EXIT_FAILURE;
}

World *cig_world_init() {
  World *result = calloc(1, sizeof(World));
  if (!result)
    return NULL;

  if (vector_init(&result->types, sizeof(TypeDesc)))
    goto err;

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
  cig_world_deinit(result);
  return NULL;
}

void cig_world_deinit(World *w) {
  if (w == NULL)
    return;

  TypeDesc *types = w->types.data;
  for (size_t i = 0; i < vector_len(&w->types); i++)
    free(types[i].identifier);
  vector_deinit(&w->types);

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

  free(w);
}

static size_t find_type(const Vector *types, const char *identifier) {
  TypeDesc *arr = types->data;
  size_t len = vector_len(types);
  for (size_t i = 0; i < len; i++)
    if (strcmp(arr[i].identifier, identifier) == 0)
      return i;

  return len;
}

int cig_world_register_type(World *w, TypeDesc *desc) {
  assert(w != NULL);
  assert(desc != NULL);

  if (find_type(&w->types, desc->identifier) < vector_len(&w->types)) {
#ifdef DEBUG
    fprintf(stderr, "%s(): Type with identifier already registered (%s).\n",
            __func__, desc->identifier);
#endif
    return EXIT_FAILURE;
  }

  if (vector_append(&w->types, desc))
    return EXIT_FAILURE;

  char *identifier = strdup(desc->identifier);
  if (!identifier) {
    vector_delete(&w->types, vector_len(&w->types) - 1);
    return EXIT_FAILURE;
  }
  ((TypeDesc *)vector_get(&w->types, vector_len(&w->types) - 1))->identifier =
      identifier;

#ifdef DEBUG
  printf("%s(): Type registered (%s).\n", __func__, desc->identifier);
#endif

  return EXIT_SUCCESS;
}

int cig_world_register_system(World *w, SystemDesc *desc) {
  struct system system;
  if (system_init(w, &system, desc))
    return EXIT_FAILURE;

  if (vector_append(&w->systems, &system)) {
    system_deinit(&system);
    return EXIT_FAILURE;
  }

#ifdef DEBUG
  printf("%s(): System registered (%s).\n", __func__, desc->identifier);
#endif

  return EXIT_SUCCESS;
}

const Entity *cig_world_spawn(World *w, size_t count, const char *types) {
  assert(w != NULL);
  assert(types != NULL);

  // Check the given types.
  /*Bitset mask;*/
  /*if (gen_mask(w, &mask, types))*/
  /*return NULL;*/

  /*// Resize the last_spawned array.*/
  /*Entity *result = realloc(w->last_spawned, sizeof(Entity) * count);*/
  /*if (!result) {*/
  /*bitset_deinit(&mask);*/
  /*return NULL;*/
  /*}*/
}
