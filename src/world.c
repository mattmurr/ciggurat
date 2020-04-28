/**
 * src/world.c
 * Copyright (c) 2020 Matthew Murray <matt@compti.me>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <ciggurat.h>
#include <mylib/mylib.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK_KB_SIZE 16
#define CHUNK_BYTE_SIZE CHUNK_KB_SIZE * 1024

struct entity_internal {
  // The storage that contains this entity's types. The storage also contains
  // the mask for the entity.
  struct storage *storage;
  // A pointer to the entity's types in the storage.
  void *ptr;
};

struct storage_layout_type_desc {
  uint32_t id;
  size_t size;
  size_t offset;
};

struct storage_layout {
  // Keeps track of the type id and corresponding size which may or may not
  // contain padding.
  struct storage_layout_type_desc *types;

  // The count of types in a family
  size_t count;

  // The total size in bytes of a single family when packed
  size_t family_size;

  // The alignment for the family, derived from the widest type
  size_t alignment;
};

struct region {
  void *ptr;
  size_t count;
};

struct storage_regions_request {
  // Pointer to the storage in context
  struct storage *storage;

  // The requests regions
  Vector regions;

  // The storage's unassigned Vector will be resized to this count on commit
  size_t new_unassigned_count;
};

struct storage {
  // The mask generated for the combination of types that this storage contains.
  Bitset mask;
  struct storage_layout layout;

  // Contains `struct region`
  LinkedList regions;

  // Contains `void *`
  Vector unassigned;

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
  // Contains `struct entity_internal`.
  Vector entities;
  // Contains `Entity`
  Vector unassigned;
  // Runtime allocated array of the last entities that were spawned.
  Entity *last_spawned;
} World;

static uint32_t system_hash(const void *system_ptr) {
  const struct system *system = system_ptr;
  return fnv1a_32_hash((const uint8_t *)system->identifier,
                       strlen(system->identifier) + 1);
}

static int system_eql(const void *a, const void *b) {
  // a and b are of the type const struct *system.
  return strcmp(((const struct system *)a)->identifier,
                ((const struct system *)b)->identifier) == 0;
}

// TODO Aligned alloc
static int region_init(struct region *result, size_t alignment) {
  *result = (struct region){0};
  // TODO The allocation size can be less depending on the family_size
  result->ptr = aligned_alloc(alignment, CHUNK_BYTE_SIZE);
  memset(result->ptr, 0, CHUNK_BYTE_SIZE);
  return result->ptr == NULL ? EXIT_FAILURE : EXIT_SUCCESS;
}

static void region_deinit(struct region *region) {
  if (region == NULL)
    return;
  free(region->ptr);
}

static const TypeDesc *get_type(World *w, int32_t id) {
  return vector_get_const(&w->types, id);
}

static size_t get_size(World *w, int32_t id) { return get_type(w, id)->size; }

static size_t get_alignment(World *w, int32_t id) {
  return get_type(w, id)->alignment;
}

static int32_t get_id(const World *w, const char *type_str) {
  TypeDesc *types = w->types.data;
  for (size_t i = 0; i < vector_len(&w->types); i++)
    if (strcmp(types[i].identifier, type_str) == 0)
      return i;

#ifdef DEBUG
  printf("%s(): Could not find a type with a matching identifier (%s), is the "
         "type registered?\n.",
         __func__, type_str);
#endif
  return -1;
}

static int calculate_layout(World *w, struct storage_layout *layout,
                            Bitset mask) {

  *layout = (struct storage_layout){0};

  layout->types =
      malloc(sizeof(struct storage_layout_type_desc) * bitset_count(&mask));
  if (!layout->types)
    return EXIT_FAILURE;

  // Figure out the alignment for the family and the largest type to be
  // packed first
  {
    size_t id;
    bitset_first(&mask, &id);
    layout->types[0].id = id;
    layout->types[0].size = get_size(w, id);
    layout->alignment = get_alignment(w, id);
  }

  for (size_t id = 0; bitset_next(&mask, &id); id++) {
    const size_t width = get_alignment(w, id);
    if (width > layout->alignment)
      layout->alignment = width;

    const size_t size = get_size(w, id);
    if (size > layout->types[0].size) {
      layout->types[0].id = id;
      layout->types[0].size = size;
    }
  }

  Bitset remaining_types;
  if (bitset_clone(&mask, &remaining_types)) {
    free(layout->types);
    return EXIT_FAILURE;
  }

  // Remove the already staged type
  bitset_excl(&remaining_types, layout->types[0].id);

  layout->count = bitset_count(&mask);

  size_t remaining_bytes =
      layout->alignment - (layout->types[0].size % layout->alignment);
  size_t i = 1;
  while (layout->count - i > 0) {
    size_t next_id;
    bitset_first(&remaining_types, &next_id);
    size_t next_size = get_size(w, next_id);

    for (size_t id = next_id; bitset_next(&remaining_types, &id); id++) {
      const size_t size = get_size(w, id);
      if (size <= remaining_bytes) {
        next_id = id;
        next_size = size;

        // Just break instantly if it fits perfectly
        if (size == remaining_bytes)
          break;
      }
    }

    if (next_size > remaining_bytes)
      layout->types[i - 1].size += remaining_bytes;

    layout->types[i] =
        (struct storage_layout_type_desc){.id = next_id, .size = next_size};
    remaining_bytes = layout->alignment - (next_size % layout->alignment);
    bitset_excl(&remaining_types, next_id);

    i++;
  }

  bitset_deinit(&remaining_types);

  for (size_t i = 0; i < layout->count; i++) {
    layout->types[i].offset = layout->family_size;
    layout->family_size += layout->types[i].size;
#ifdef DEBUG
    printf("%s(): type ID: %i, size: %zi offset: %zu\n", __func__,
           layout->types[i].id, layout->types[i].size, layout->types[i].offset);
#endif
  }
#ifdef DEBUG
  printf("%s(): family size: %zu, alignment: %zu\n", __func__,
         layout->family_size, layout->alignment);
#endif

  return EXIT_SUCCESS;
}

static int storage_init(World *w, struct storage *result, Bitset mask) {
  *result = (struct storage){0};

  result->regions = linked_list_init();

  if (vector_init(&result->unassigned, sizeof(struct region)))
    goto err;

  if (hash_map_init(&result->systems, system_hash, system_eql,
                    sizeof(struct system), 0))
    goto err;

  if (calculate_layout(w, &result->layout, mask))
    goto err;

  result->mask = mask;

  return EXIT_SUCCESS;

err:
  hash_map_deinit(&result->systems);
  vector_deinit(&result->unassigned);
  linked_list_deinit(&result->regions);

  return EXIT_FAILURE;
}

static void storage_deinit(struct storage *storage) {
  if (storage == NULL)
    return;

  if (storage->layout.family_size > 0) {
    // For each region, deinitialize
    LinkedListNode *node = storage->regions.first;
    if (node) {
      do {
        region_deinit((struct region *)node->data);
      } while ((node = node->next));
    }
  }

  linked_list_deinit(&storage->regions);

  vector_deinit(&storage->unassigned);
  hash_map_deinit(&storage->systems);
  bitset_deinit(&storage->mask);

  free(storage->layout.types);
}

static void system_deinit(struct system *system) {
  bitset_deinit(&system->must_not_have);
  bitset_deinit(&system->must_have);

  free(system->types);

  free(system->identifier);
}

static int is_match(const Bitset mask, const Bitset must_have,
                    const Bitset must_not_have) {
  return (!bitset_intersects(&mask, &must_not_have)) &&
         bitset_is_proper_subset(&mask, &must_have);
}

static int storage_find_matches(World *w, struct storage *storage) {
  struct system *systems = w->systems.data;
  for (size_t i = 0; i < vector_len(&w->systems); i++) {
    if (!is_match(storage->mask, systems[i].must_have,
                  systems[i].must_not_have))
      continue;

    if (hash_map_put(&storage->systems, &systems[i], NULL) ||
        hash_map_put(&systems[i].storages, storage, NULL)) {
      size_t j = i;
      while (j > 0) {
        hash_map_delete(&storage->systems, &systems[j]);
        hash_map_delete(&systems[j].storages, storage);
        j--;
      }
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}

static struct storage *get_storage(World *w, Bitset mask) {
  int has_existing;
  const HashMapKV *kv = hash_map_get_or_put(&w->storages, &mask, &has_existing);

  // NULL means that `hash_map_get_or_put()` operation failed
  if (!kv)
    return NULL;

  if (has_existing)
    return kv->value;

  struct storage storage;
  if (storage_init(w, &storage, mask))
    return NULL;

  hash_map_kv_assign(&w->storages, kv, &storage);

  storage_find_matches(w, kv->value);

  return kv->value;
}

static int prepend_new_region(struct storage *storage) {
  struct region region;
  if (region_init(&region, storage->layout.alignment))
    return EXIT_FAILURE;

  if (linked_list_prepend(&storage->regions, &region, sizeof(struct region))) {
    region_deinit(&region);
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

static int storage_request_regions(struct storage *storage,
                                   struct storage_regions_request *result,
                                   size_t count) {
  *result = (struct storage_regions_request){0};
  result->storage = storage;

  if (vector_init(&result->regions, sizeof(struct region)))
    return EXIT_FAILURE;

  // Handle the zero sized family
  if (storage->layout.family_size == 0) {
    struct region region = {
        .ptr = NULL,
        .count = count,
    };
    if (linked_list_prepend(&storage->regions, &region,
                            sizeof(struct region))) {
      vector_deinit(&result->regions);
      return EXIT_FAILURE;
    }

    if (vector_append(&result->regions, storage->regions.first)) {
      linked_list_delete(&storage->regions, storage->regions.first);
      vector_deinit(&result->regions);
      return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
  }

  size_t i = 0;
  const size_t unassigned_count = vector_len(&storage->unassigned);

  // Keep track of the resulting capacity after we have taken from `unassigned`
  result->new_unassigned_count = unassigned_count;
  while (result->new_unassigned_count > 0) {
    struct region *region =
        vector_get(&storage->unassigned, result->new_unassigned_count - 1);
    if (!vector_append(&result->regions, region)) {
      vector_deinit(&result->regions);
      return EXIT_FAILURE;
    }

    i++;
    result->new_unassigned_count--;
  }

  while (i < count) {
    LinkedListNode *node = storage->regions.first;
    const size_t families_per_region =
        (CHUNK_BYTE_SIZE / storage->layout.family_size);

    // Create a new region if the first node in the list is NULL or if the
    // region is full
    if (!node ||
        (families_per_region - ((struct region *)node->data)->count) == 0) {
      if (prepend_new_region(storage))
        goto err;

      node = storage->regions.first;
    }

    struct region *region = node->data;

    // Get the current offset for the region
    size_t offset = region->count * storage->layout.family_size;

    // How many more families can fit into the region
    size_t free_count = families_per_region - region->count;

    // And how many will we actually use
    size_t j = free_count < count - i ? free_count : count - i;

    struct region to_append = {.ptr = region->ptr + offset, .count = j};
    if (vector_append(&result->regions, &to_append))
      goto err;

    region->count += j;
    i += j;
  }

  return EXIT_SUCCESS;

err:;
  // TODO It's probably more sensible to send the newly initialized regions to
  // the unassigned Vector

  // Check if we allocated any new regions
  size_t j = unassigned_count - result->new_unassigned_count;
  if (vector_len(&result->regions) > j) {
    // Go through the result and deinitialize regions
    for (++j; j < i; j++) {
      LinkedListNode *node = linked_list_pop_first(&storage->regions);
      region_deinit(node->data);
      linked_list_node_deinit(node);
    }
  }

  vector_deinit(&result->regions);
  return EXIT_FAILURE;
}

static void storage_unassign_regions(struct storage *storage,
                                     struct region *regions, size_t count) {
  // Loop through the regions and attempt to append them into the unassigned
  // Vector, If we encounter an error, more than likely we are OOM so instead
  // just free the region
  size_t i = 0;
  for (; i < count; i++)
    if (vector_append(&storage->unassigned, &regions[i]))
      break;

  for (; i < count; i++)
    free(regions[i].ptr);
}

static void
storage_regions_request_commit(struct storage_regions_request *request,
                               int commit) {
  if (commit) {
    if (request->new_unassigned_count <
        vector_len(&request->storage->unassigned))
      vector_resize(&request->storage->unassigned,
                    request->new_unassigned_count);
#ifdef DEBUG
    printf("%s(): Committed modification of the storage.\n", __func__);
#endif
  } else {
    storage_unassign_regions(request->storage, request->regions.data,
                             vector_len(&request->regions));
  }

  vector_deinit(&request->regions);
}

// Count how many types should be in this string of types by counting commas + 1
static int count_types(const char *types_str) {
  size_t result;
  char c;
  // Checks if the current char is comma
  for (result = 0; (c = types_str[result]); c == ',' ? result++ : *types_str++)
    ;
  return result + 1;
}

// Splits a comma-seperated string of types into an array of token strings
// Must also provide a size_t pointer for the size of the array returned
static char **tokenize(const char *str, size_t *size) {
  char **result = NULL;

  // Duplicate types_str and remove any spaces from the str first
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

  // Keep track of how many entries are in the result array
  *size = 0;

  // Split into an array of chars
  char *token = strtok(without_whitespace, delimiters);

  while (token != NULL) {
    // Attempt to realloc the result
    char **new_arr = realloc(result, sizeof(char *) * (*size + 1));
    if (!new_arr)
      goto err;

    // Assign
    result = new_arr;

    // Duplicate the token into the result
    result[*size] = strdup(token);
    if (!result[*size])
      goto err;
    (*size)++;

    // Attempt to get the next token
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
    fprintf(stderr,
            "%s(): There are more types requested than types registered in the "
            "world. [%s]\n.",
            __func__, types_str);

  } else {
    TypeDesc *types = w->types.data;
    for (size_t i = 0; i < size; i++) {
      for (size_t j = 0; j < vector_len(&w->types); j++)
        // Call the `f` function pointer to generate the mask/s
        if (!f(mask, types[j].identifier, tokens[i], j, e))
          goto next;

      result = EXIT_FAILURE;
      fprintf(stderr,
              "%s(): Requested type (%s) does not exist in the world.\n",
              __func__, tokens[i]);

    next:;
    }
  }

  // Ensure that we free the tokens
  for (size_t i = 0; i < size; i++)
    free(tokens[i]);
  free(tokens);

  return result;
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

static int system_init(World *w, struct system *result, SystemDesc *desc) {
  *result = (struct system){0};

  // Loop through all existing systems to ensure that there is not already a
  // system registered with the same identifier
  SystemDesc *systems = w->systems.data;
  for (size_t i = 0; i < vector_len(&w->systems); i++)
    if (strcmp(systems[i].identifier, desc->identifier) == 0) {

      fprintf(stderr, "%s(): System with identifier already registered(%s).\n",
              __func__, desc->identifier);
      return EXIT_FAILURE;
    }

  result->identifier = strdup(desc->identifier);
  if (!result->identifier)
    return EXIT_FAILURE;

  {
    // Allocate memory for the `types` array.
    size_t capacity = count_types(desc->requirements) * sizeof(int32_t);
    result->types = calloc(capacity, sizeof(int32_t));
    if (!result->types)
      goto err;
  }

  {
    size_t registered_type_count = vector_len(&w->types);
    // Initialize the masks.
    if (bitset_init(&result->must_have, registered_type_count) ||
        bitset_init(&result->must_not_have, registered_type_count))
      goto err;
  }

  {
    // Create an array with both masks to pass into `populate_mask()`
    Bitset masks[2] = {result->must_have, result->must_not_have};
    // and a copy of the pointer for the types array.
    int32_t *types = result->types;

    if (populate_mask(w, masks, generate_system_masks, desc->requirements,
                      &types))
      goto err;
  }

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

  if (vector_init(&result->entities, sizeof(struct entity_internal)))
    goto err;

  if (vector_init(&result->unassigned, sizeof(Entity)))
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
  vector_deinit(&w->unassigned);
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
    fprintf(stderr, "%s(): Type with identifier already registered (%s).\n",
            __func__, desc->identifier);
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

static void system_find_matches(World *w, struct system *system) {
  HashMapIterator iter = hash_map_iter(&w->storages);
  const HashMapKV *kv;
  while ((kv = hash_map_next(&iter))) {
    if (!is_match(((struct storage *)kv->value)->mask, system->must_have,
                  system->must_not_have))
      continue;

    if (hash_map_put(&system->storages, kv->value, NULL) ||
        hash_map_put(&((struct storage *)kv->value)->systems, system, NULL)) {
    }
  }
}

int cig_world_register_system(World *w, SystemDesc *desc) {
  struct system system;
  if (system_init(w, &system, desc))
    return EXIT_FAILURE;

  if (vector_append(&w->systems, &system)) {
    system_deinit(&system);
    return EXIT_FAILURE;
  }

  system_find_matches(w, &system);

#ifdef DEBUG
  printf("%s(): System registered (%s).\n", __func__, desc->identifier);
#endif

  return EXIT_SUCCESS;
}

static int generate_entity_mask(Bitset *mask, const char *type,
                                const char *token, int32_t id, void *e) {
  if (strcmp(token, type) == 0) {
    bitset_incl(mask, id);
    return EXIT_SUCCESS;
  }

  return EXIT_FAILURE;
}

static size_t get_offset(const World *w, struct storage *storage, int32_t id) {
  // Iterate the storage's layout to find the id
  for (int32_t i = 0; i < storage->layout.count; i++)
    if (id == storage->layout.types[i].id)
      return storage->layout.types[i].offset;

#ifdef DEBUG
  fprintf(stderr, "%s(): Storage does not contain a type with the ID (%i).\n",
          __func__, id);
#endif
  return -1;
}

static int assign_regions(World *w, struct storage *storage, Bitset mask,
                          size_t count) {
  struct storage_regions_request request;
  if (storage_request_regions(storage, &request, count))
    return EXIT_FAILURE;

  size_t i = 0;
  for (size_t k = 0; k < vector_len(&request.regions); k++) {
    struct region *region = vector_get(&request.regions, k);

    size_t j = 0;
    while (j < region->count) {
      struct entity_internal *e = vector_get(&w->entities, w->last_spawned[i]);

      // Check if the entity has existing storage, this means that there may be
      // types that need to be moved into the new storage
      if (e->storage) {
        struct storage *old_storage = e->storage;

        // Get the intersection between the old and new storage masks
        Bitset intersection;
        if (bitset_intersect(&old_storage->mask, &storage->mask, &intersection))
          goto err;

        // For each of the intersecting types, copy the type from the old
        // storage to the new storage
        for (size_t id = 0; bitset_next(&intersection, &id); id++) {
          void *src = e->ptr + get_offset(w, old_storage, id);
          void *dest = e->ptr + get_offset(w, storage, id);

          // Get the size of the type to copy
          size_t size = get_size(w, id);
          memcpy(src, dest, size);
        }

        bitset_deinit(&intersection);
      }

      // Assign the entities new components and storage pointers
      size_t offset = j * storage->layout.family_size;

      e->ptr = region->ptr + offset;
      e->storage = storage;

      i++;
      j++;
    }
  }

  storage_regions_request_commit(&request, 1);
  return EXIT_SUCCESS;

err:
  storage_regions_request_commit(&request, 0);
  return EXIT_FAILURE;
}

const Entity *cig_world_spawn(World *w, size_t count, const char *types_str) {
  assert(w != NULL);
  assert(types_str != NULL);

  size_t types_count = count_types(types_str);

  Entity *result = realloc(w->last_spawned, sizeof(Entity) * count);
  if (!result)
    return NULL;

  Bitset mask;
  if (bitset_init(&mask, types_count))
    goto err;

  if (populate_mask(w, &mask, generate_entity_mask, types_str, NULL))
    goto err;

  struct storage *storage = get_storage(w, mask);
  if (!storage)
    goto err;

  const size_t unassigned_count = vector_len(&w->unassigned);
  size_t new_unassigned_count = unassigned_count;

  // `i` is used to keep track of how many entities we have sorted out
  size_t i = 0;
  // Take as many entities as possible from world->recycled first
  while (new_unassigned_count > 0)
    result[i++] =
        *((Entity *)vector_get(&w->unassigned, --new_unassigned_count));

  // Make space for the new entities
  if (vector_resize(&w->entities, count - i))
    goto err;

  struct entity_internal e = {0};
  while (i < count) {
    vector_append(&w->entities, &e);
    // Make sure `next_entity` is iterated
    result[i++] = w->next_entity++;
  }

  w->last_spawned = result;

  // How many did we take from recycled
  size_t recycled_count = unassigned_count - new_unassigned_count;
  size_t new_count = count - recycled_count;
  if (assign_regions(w, storage, mask, count)) {
    // Reset everything back to what it was before.
    vector_resize(&w->entities, vector_len(&w->entities) - new_count);
    w->next_entity -= new_count;
    goto err;
  }

  // If we took anything from recycled then be sure to shrink it down to it's
  // new capacity
  if (recycled_count > 0)
    vector_resize(&w->unassigned, new_unassigned_count);

#ifdef DEBUG
  printf("%s(): Spawned (%zu) entities with types [%s].\nRecycled: %zu\nNew: "
         "%zu\n",
         __func__, count, types_str, recycled_count, new_count);
#endif
  return w->last_spawned;

err:
  bitset_deinit(&mask);
  free(result);

  return NULL;
}

void *cig_get_component(const World *w, const Entity e, const char *type_str) {
  assert(w != NULL);
  assert(type_str != NULL);

  const struct entity_internal *e_internal = vector_get_const(&w->entities, e);
  if (!e_internal->ptr) {
#ifdef DEBUG
    fprintf(stderr, "%s(): Entity (%zu) contains no components.\n", __func__,
            e);
#endif
    return NULL;
  }

  // If the entity has components, there should also be a storage
  assert(e_internal->storage != NULL);

  const int32_t id = get_id(w, type_str);
  if (id < 0) {
#ifdef DEBUG
    fprintf(stderr,
            "%s(): Attempted to get component from entity (%zu), there is no "
            "type with the identifier (%s).\n",
            __func__, e, type_str);
#endif
    return NULL;
  }

  if (!bitset_has(&e_internal->storage->mask, id)) {
#ifdef DEBUG
    fprintf(stderr, "%s(): Entity (%zu) does not have the component type (%s)",
            __func__, e, type_str);
#endif
    return NULL;
  }

  const size_t offset = get_offset(w, e_internal->storage, id);
  if (offset == -1)
    return NULL;

#ifdef DEBUG
  printf("%s(): Returning pointer to component type (%s) belonging to entity "
         "(%zu).\n",
         __func__, type_str, e);
#endif

  return e_internal->ptr + offset;
}
