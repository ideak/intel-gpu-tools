/*
 * Copyright © 2009, 2021 Intel Corporation
 * Copyright © 1988-2004 Keith Packard and Bart Massey.
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
 * Except as contained in this notice, the names of the authors
 * or their institutions shall not be used in advertising or
 * otherwise to promote the sale, use or other dealings in this
 * Software without prior written authorization from the
 * authors.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *    Keith Packard <keithp@keithp.com>
 */

#include <assert.h>
#include <stdlib.h>

#include "igt_map.h"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof(array[0]))

/*
 * From Knuth -- a good choice for hash/rehash values is p, p-2 where
 * p and p-2 are both prime.  These tables are sized to have an extra 10%
 * free to avoid exponential performance degradation as the hash table fills
 */

static const uint32_t deleted_key_value;
static const void *deleted_key = &deleted_key_value;

static const struct {
	uint32_t max_entries, size, rehash;
} hash_sizes[] = {
	{ 2,		5,		3	  },
	{ 4,		7,		5	  },
	{ 8,		13,		11	  },
	{ 16,		19,		17	  },
	{ 32,		43,		41        },
	{ 64,		73,		71        },
	{ 128,		151,		149       },
	{ 256,		283,		281       },
	{ 512,		571,		569       },
	{ 1024,		1153,		1151      },
	{ 2048,		2269,		2267      },
	{ 4096,		4519,		4517      },
	{ 8192,		9013,		9011      },
	{ 16384,	18043,		18041     },
	{ 32768,	36109,		36107     },
	{ 65536,	72091,		72089     },
	{ 131072,	144409,		144407    },
	{ 262144,	288361,		288359    },
	{ 524288,	576883,		576881    },
	{ 1048576,	1153459,	1153457   },
	{ 2097152,	2307163,	2307161   },
	{ 4194304,	4613893,	4613891   },
	{ 8388608,	9227641,	9227639   },
	{ 16777216,	18455029,	18455027  },
	{ 33554432,	36911011,	36911009  },
	{ 67108864,	73819861,	73819859  },
	{ 134217728,	147639589,	147639587 },
	{ 268435456,	295279081,	295279079 },
	{ 536870912,	590559793,	590559791 },
	{ 1073741824,	1181116273,	1181116271},
	{ 2147483648ul,	2362232233ul,	2362232231ul}
};

static int
entry_is_free(const struct igt_map_entry *entry)
{
	return entry->key == NULL;
}

static int
entry_is_deleted(const struct igt_map_entry *entry)
{
	return entry->key == deleted_key;
}

static int
entry_is_present(const struct igt_map_entry *entry)
{
	return entry->key != NULL && entry->key != deleted_key;
}

/**
 * igt_map_create:
 * @hash_function: function that maps key to 32b hash
 * @key_equals_function: function that compares given hashes
 *
 * Function creates a map and initializes it with given @hash_function and
 * @key_equals_function.
 *
 * Returns: pointer to just created map
 */
struct igt_map *
igt_map_create(uint32_t (*hash_function)(const void *key),
	       int (*key_equals_function)(const void *a, const void *b))
{
	struct igt_map *map;

	map = malloc(sizeof(*map));
	if (map == NULL)
		return NULL;

	map->size_index = 0;
	map->size = hash_sizes[map->size_index].size;
	map->rehash = hash_sizes[map->size_index].rehash;
	map->max_entries = hash_sizes[map->size_index].max_entries;
	map->hash_function = hash_function;
	map->key_equals_function = key_equals_function;
	map->table = calloc(map->size, sizeof(*map->table));
	map->entries = 0;
	map->deleted_entries = 0;

	if (map->table == NULL) {
		free(map);
		return NULL;
	}

	return map;
}

/**
 * igt_map_destroy:
 * @map: igt_map pointer
 * @delete_function: function that frees data in igt_map_entry
 *
 * Frees the given hash table. If @delete_function is passed, it gets called
 * on each entry present before freeing.
 */
void
igt_map_destroy(struct igt_map *map,
		void (*delete_function)(struct igt_map_entry *entry))
{
	if (!map)
		return;

	if (delete_function) {
		struct igt_map_entry *entry;

		igt_map_foreach(map, entry) {
			delete_function(entry);
		}
	}
	free(map->table);
	free(map);
}

/**
 * igt_map_search:
 * @map: igt_map pointer
 * @key: pointer to searched key
 *
 * Finds a map entry's data with the given @key.
 *
 * Returns: data pointer if the entry was found, %NULL otherwise.
 * Note that it may be modified by the user.
 */
void *
igt_map_search(struct igt_map *map, const void *key)
{
	uint32_t hash = map->hash_function(key);
	struct igt_map_entry *entry;

	entry = igt_map_search_pre_hashed(map, hash, key);
	return entry ? entry->data : NULL;
}

/**
 * igt_map_search_entry:
 * @map: igt_map pointer
 * @key: pointer to searched key
 *
 * Finds a map entry with the given @key.
 *
 * Returns: map entry or %NULL if no entry is found.
 * Note that the data pointer may be modified by the user.
 */
struct igt_map_entry *
igt_map_search_entry(struct igt_map *map, const void *key)
{
	uint32_t hash = map->hash_function(key);

	return igt_map_search_pre_hashed(map, hash, key);
}

/**
 * igt_map_search_pre_hashed:
 * @map: igt_map pointer
 * @hash: hash of @key
 * @key: pointer to searched key
 *
 * Finds a map entry with the given @key and @hash of that key.
 *
 * Returns: map entry or %NULL if no entry is found.
 * Note that the data pointer may be modified by the user.
 */
struct igt_map_entry *
igt_map_search_pre_hashed(struct igt_map *map, uint32_t hash,
			  const void *key)
{
	uint32_t start_hash_address = hash % map->size;
	uint32_t hash_address = start_hash_address;

	do {
		uint32_t double_hash;

		struct igt_map_entry *entry = map->table + hash_address;

		if (entry_is_free(entry)) {
			return NULL;
		} else if (entry_is_present(entry) && entry->hash == hash) {
			if (map->key_equals_function(key, entry->key)) {
				return entry;
			}
		}

		double_hash = 1 + hash % map->rehash;

		hash_address = (hash_address + double_hash) % map->size;
	} while (hash_address != start_hash_address);

	return NULL;
}

static void
igt_map_rehash(struct igt_map *map, int new_size_index)
{
	struct igt_map old_map;
	struct igt_map_entry *table, *entry;

	if (new_size_index >= ARRAY_SIZE(hash_sizes))
		return;

	table = calloc(hash_sizes[new_size_index].size, sizeof(*map->table));
	if (table == NULL)
		return;

	old_map = *map;

	map->table = table;
	map->size_index = new_size_index;
	map->size = hash_sizes[map->size_index].size;
	map->rehash = hash_sizes[map->size_index].rehash;
	map->max_entries = hash_sizes[map->size_index].max_entries;
	map->entries = 0;
	map->deleted_entries = 0;

	igt_map_foreach(&old_map, entry) {
		igt_map_insert_pre_hashed(map, entry->hash,
					     entry->key, entry->data);
	}

	free(old_map.table);
}

/**
 * igt_map_insert:
 * @map: igt_map pointer
 * @data: data to be store
 * @key: pointer to searched key
 *
 * Inserts the @data indexed by given @key into the map. If the @map already
 * contains an entry with the @key, it will be replaced. To avoid memory leaks,
 * perform a search before inserting.
 *
 * Note that insertion may rearrange the table on a resize or rehash,
 * so previously found hash entries are no longer valid after this function.
 *
 * Returns: pointer to just inserted entry
 */
struct igt_map_entry *
igt_map_insert(struct igt_map *map, const void *key, void *data)
{
	uint32_t hash = map->hash_function(key);

	/* Make sure nobody tries to add one of the magic values as a
	 * key. If you need to do so, either do so in a wrapper, or
	 * store keys with the magic values separately in the struct
	 * igt_map.
	 */
	assert(key != NULL);

	return igt_map_insert_pre_hashed(map, hash, key, data);
}

/**
 * igt_map_insert_pre_hashed:
 * @map: igt_map pointer
 * @hash: hash of @key
 * @data: data to be store
 * @key: pointer to searched key
 *
 * Inserts the @data indexed by given @key and @hash of that @key into the map.
 * If the @map already contains an entry with the @key, it will be replaced.
 * To avoid memory leaks, perform a search before inserting.
 *
 * Note that insertion may rearrange the table on a resize or rehash,
 * so previously found hash entries are no longer valid after this function.
 *
 * Returns: pointer to just inserted entry
 */
struct igt_map_entry *
igt_map_insert_pre_hashed(struct igt_map *map, uint32_t hash,
			  const void *key, void *data)
{
	uint32_t start_hash_address, hash_address;
	struct igt_map_entry *available_entry = NULL;

	if (map->entries >= map->max_entries) {
		igt_map_rehash(map, map->size_index + 1);
	} else if (map->deleted_entries + map->entries >= map->max_entries) {
		igt_map_rehash(map, map->size_index);
	}

	start_hash_address = hash % map->size;
	hash_address = start_hash_address;
	do {
		struct igt_map_entry *entry = map->table + hash_address;
		uint32_t double_hash;

		if (!entry_is_present(entry)) {
			/* Stash the first available entry we find */
			if (available_entry == NULL)
				available_entry = entry;
			if (entry_is_free(entry))
				break;
		}

		/* Implement replacement when another insert happens
		 * with a matching key.  This is a relatively common
		 * feature of hash tables, with the alternative
		 * generally being "insert the new value as well, and
		 * return it first when the key is searched for".
		 *
		 * Note that the hash table doesn't have a delete
		 * callback.  If freeing of old data pointers is
		 * required to avoid memory leaks, perform a search
		 * before inserting.
		 */
		if (!entry_is_deleted(entry) &&
		    entry->hash == hash &&
		    map->key_equals_function(key, entry->key)) {
			entry->key = key;
			entry->data = data;
			return entry;
		}


		double_hash = 1 + hash % map->rehash;

		hash_address = (hash_address + double_hash) % map->size;
	} while (hash_address != start_hash_address);

	if (available_entry) {
		if (entry_is_deleted(available_entry))
			map->deleted_entries--;
		available_entry->hash = hash;
		available_entry->key = key;
		available_entry->data = data;
		map->entries++;
		return available_entry;
	}

	/* We could hit here if a required resize failed. An unchecked-malloc
	 * application could ignore this result.
	 */
	return NULL;
}

/**
 * igt_map_remove:
 * @map: igt_map pointer
 * @key: pointer to searched key
 * @delete_function: function that frees data in igt_map_entry
 *
 * Function searches for an entry with a given @key, and removes it from
 * the map. If @delete_function is passed, it will be called on removed entry.
 *
 * If the caller has previously found a struct igt_map_entry pointer,
 * (from calling igt_map_search() or remembering it from igt_map_insert()),
 * then igt_map_remove_entry() can be called instead to avoid an extra search.
 */
void
igt_map_remove(struct igt_map *map, const void *key,
		void (*delete_function)(struct igt_map_entry *entry))
{
	struct igt_map_entry *entry;

	entry = igt_map_search_entry(map, key);
	if (delete_function)
		delete_function(entry);

	igt_map_remove_entry(map, entry);
}

/**
 * igt_map_remove_entry:
 * @map: igt_map pointer
 * @entry: pointer to map entry
 *
 * Function deletes the given hash entry.
 *
 * Note that deletion doesn't otherwise modify the table, so an iteration over
 * the map deleting entries is safe.
 */
void
igt_map_remove_entry(struct igt_map *map, struct igt_map_entry *entry)
{
	if (!entry)
		return;

	entry->key = deleted_key;
	map->entries--;
	map->deleted_entries++;
}

/**
 * igt_map_next_entry:
 * @map: igt_map pointer
 * @entry: pointer to map entry, %NULL for the first map entry
 *
 * This function is an iterator over the hash table.
 * Note that an iteration over the table is O(table_size) not O(entries).
 *
 * Returns: pointer to the next entry
 */
struct igt_map_entry *
igt_map_next_entry(struct igt_map *map, struct igt_map_entry *entry)
{
	if (entry == NULL)
		entry = map->table;
	else
		entry = entry + 1;

	for (; entry != map->table + map->size; entry++) {
		if (entry_is_present(entry)) {
			return entry;
		}
	}

	return NULL;
}

/**
 * igt_map_random_entry:
 * @map: igt_map pointer
 * @predicate: filtering entries function
 *
 * Functions returns random entry from the map. This may be useful in
 * implementing random replacement (as opposed to just removing everything)
 * in caches based on this hash table implementation. @predicate may be used to
 * filter entries, or may be set to %NULL for no filtering.
 *
 * Returns: pointer to the randomly chosen map entry
 */
struct igt_map_entry *
igt_map_random_entry(struct igt_map *map,
		     int (*predicate)(struct igt_map_entry *entry))
{
	struct igt_map_entry *entry;
	uint32_t i = random() % map->size;

	if (map->entries == 0)
		return NULL;

	for (entry = map->table + i; entry != map->table + map->size; entry++) {
		if (entry_is_present(entry) &&
		    (!predicate || predicate(entry))) {
			return entry;
		}
	}

	for (entry = map->table; entry != map->table + i; entry++) {
		if (entry_is_present(entry) &&
		    (!predicate || predicate(entry))) {
			return entry;
		}
	}

	return NULL;
}
