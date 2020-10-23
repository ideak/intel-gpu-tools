/*
 * Copyright © 2009,2021 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *
 */

#ifndef IGT_MAP_H
#define IGT_MAP_H

#include <inttypes.h>

/**
 * SECTION:igt_map
 * @short_description: a linear-reprobing hashmap implementation
 * @title: IGT Map
 * @include: igt_map.h
 *
 * Implements an open-addressing, linear-reprobing hash table.
 *
 * For more information, see:
 * http://cgit.freedesktop.org/~anholt/hash_table/tree/README
 *
 * Example usage:
 *
 *|[<!-- language="C" -->
 * #define GOLDEN_RATIO_PRIME_32 0x9e370001UL
 *
 * static inline uint32_t hash_identifier(const void *val)
 * {
 *	uint32_t hash = *(uint32_t *) val;
 *
 *	hash = hash * GOLDEN_RATIO_PRIME_32;
 *	return hash;
 * }
 *
 * static int equal_identifiers(const void *a, const void *b)
 * {
 *	uint32_t *key1 = (uint32_t *) a, *key2 = (uint32_t *) b;
 *
 *	return *key1 == *key2;
 * }
 *
 * static void free_func(struct igt_map_entry *entry)
 * {
 * 	free(entry->data);
 * }
 *
 * struct igt_map *map;
 *
 * struct record {
 *      int foo;
 *      uint32_t unique_identifier;
 * };
 *
 * struct record *r1, r2, *record;
 * struct igt_map_entry *entry;
 *
 * r1 = malloc(sizeof(struct record));
 * map = igt_map_create(hash_identifier, equal_identifiers);
 * igt_map_insert(map, &r1->unique_identifier, r1);
 * igt_map_insert(map, &r2.unique_identifier, &r2);
 *
 * igt_map_foreach(map, entry) {
 * 	record = entry->data;
 * 	printf("key: %u, foo: %d\n", *(uint32_t *) entry->key, record->foo);
 * }
 *
 * record = igt_map_search(map, &r1->unique_identifier);
 * entry = igt_map_search_entry(map, &r2.unique_identifier);
 *
 * igt_map_remove(map, &r1->unique_identifier, free_func);
 * igt_map_remove_entry(map, entry);
 *
 *  igt_map_destroy(map, NULL);
 * ]|
 */

struct igt_map_entry {
	uint32_t hash;
	const void *key;
	void *data;
};

struct igt_map {
	struct igt_map_entry *table;
	uint32_t (*hash_function)(const void *key);
	int (*key_equals_function)(const void *a, const void *b);
	uint32_t size;
	uint32_t rehash;
	uint32_t max_entries;
	uint32_t size_index;
	uint32_t entries;
	uint32_t deleted_entries;
};

struct igt_map *
igt_map_create(uint32_t (*hash_function)(const void *key),
	       int (*key_equals_function)(const void *a, const void *b));
void
igt_map_destroy(struct igt_map *map,
		void (*delete_function)(struct igt_map_entry *entry));

struct igt_map_entry *
igt_map_insert(struct igt_map *map, const void *key, void *data);

void *
igt_map_search(struct igt_map *map, const void *key);

struct igt_map_entry *
igt_map_search_entry(struct igt_map *map, const void *key);

void
igt_map_remove(struct igt_map *map, const void *key,
	       void (*delete_function)(struct igt_map_entry *entry));

void
igt_map_remove_entry(struct igt_map *map, struct igt_map_entry *entry);

struct igt_map_entry *
igt_map_next_entry(struct igt_map *map, struct igt_map_entry *entry);

struct igt_map_entry *
igt_map_random_entry(struct igt_map *map,
		     int (*predicate)(struct igt_map_entry *entry));
/**
 * igt_map_foreach
 * @map: igt_map pointer
 * @entry: igt_map_entry pointer
 *
 * Macro is a loop, which iterates through each map entry. Inside a
 * loop block current element is accessible by the @entry pointer.
 *
 * This foreach function is safe against deletion (which just replaces
 * an entry's data with the deleted marker), but not against insertion
 * (which may rehash the table, making entry a dangling pointer).
 */
#define igt_map_foreach(map, entry)				\
	for (entry = igt_map_next_entry(map, NULL);		\
	     entry != NULL;					\
	     entry = igt_map_next_entry(map, entry))

/* Alternate interfaces to reduce repeated calls to hash function. */
struct igt_map_entry *
igt_map_search_pre_hashed(struct igt_map *map,
			     uint32_t hash,
			     const void *key);

struct igt_map_entry *
igt_map_insert_pre_hashed(struct igt_map *map,
			     uint32_t hash,
			     const void *key, void *data);

#endif
