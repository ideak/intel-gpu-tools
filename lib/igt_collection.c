/*
 * Copyright © 2020 Intel Corporation
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
 */

#include "igt.h"
#include "igt_collection.h"

/**
 * SECTION:igt_collection
 * @short_description: Generic combinatorics library
 * @title: Collection
 * @include: igt.h
 *
 * # Generic combinatorics library.
 *
 * Supports:
 * - subsets
 * - combinations
 * - variations with repetitions
 * - variations without repetitions
 *
 * ## Subsets
 *
 * Let A = { 1, 2, 3 }
 *
 * With subset size == 2 we got subsets with number of elements <= subset size:
 * {}
 * { 1 }
 * { 2 }
 * { 3 }
 * { 1, 2 }
 * { 1, 3 }
 * { 2, 3 }
 *
 * ## Combinations
 *
 * Let A = { 1, 2, 3 }
 *
 * With subset size == 2 we got subsets with number of elements == subset size:
 * { 1, 2 }
 * { 1, 3 }
 * { 2, 3 }
 *
 * So it is similar to subset extraction but targeted to single subset size.
 *
 * ## Variations with repetitions
 *
 * Let A = { 0, 1 }
 *
 * With result size == 3 we got result tuples:
 *
 * ( 0, 0, 0 )
 * ( 0, 0, 1 )
 * ( 0, 1, 0 )
 * ( 0, 1, 1 )
 * ( 1, 0, 0 )
 * ( 1, 0, 1 )
 * ( 1, 1, 0 )
 * ( 1, 1, 1 )
 *
 * ## Variations without repetitions
 *
 * Let A = { 1, 2, 3 }
 *
 * With subset size == 2 we got tuples:
 *
 * (1, 2)
 * (1, 3)
 * (2, 1)
 * (2, 3)
 * (3, 1)
 * (3, 2)
 *
 * # Usage examples:
 *
 * ## iterator is manually controlled:
 *
 * |[<!-- language="C" -->
 * struct igt_collection *set;
 * struct igt_collection *subset;
 * struct igt_collection_iter *iter;
 *
 * int i;
 * set = igt_collection_create(4);
 * iter = igt_collection_iter_init(set, 2, SUBSET);
 * //iter = igt_collection_iter_init(set, 2, COMBINATION);
 * //iter = igt_collection_iter_init(set, 2, VARIATION_R);
 * //iter = igt_collection_iter_init(set, 2, VARIATION_NR);
 *
 * for (i = 0; i < set->size; i++) {
 *      igt_collection_set_value(set, i, i + 1);
 *      igt_collection_set_pointer(set, i, &i + i);
 * }
 *
 * while ((subset = igt_collection_iter_next(iter))) {
 *      // --- do sth with subset ---
 *      // --- subset is a part of iterator, so don't free it! ---
 * }
 *
 * igt_collection_iter_destroy(iter);
 * igt_collection_destroy(set);
 * ]|
 *
 * ## iterator is created and destroyed inside helper macros:
 * |[<!-- language="C" -->
 * struct igt_collection *set;
 * struct igt_collection *subset, *result;
 * struct igt_collection_data *data;
 *
 * for_each_subset(subset, subset_size, set)
 *       // --- do sth with subset ---
 *
 * for_each_combination(subset, subset_size, set)
 *       // --- do sth with subset ---
 *
 * for_each_variation_r(result, result_size, set)
 *       // --- do sth with result ---
 *
 * for_each_variation_nr(result, result_size, set)
 *       // --- do sth with result ---
 *
 * // macro for iteration over set data - for_each_collection_data()
 * for_each_subset(subset, subset_size, set)
 *       for_each_collection_data(data, subset)
 *             printf("v: %d, p: %p\n", data->value, data->ptr);
 * ]|
 */

struct igt_collection_iter {
	const struct igt_collection *set;
	enum igt_collection_iter_algo algorithm;
	bool init;
	int result_size;
	struct igt_collection result;

	/* Algorithms state */
	struct {
		uint32_t result_bits;
		int current_result_size;
		int idxs[IGT_COLLECTION_MAXSIZE];
	} data;
};

/**
 * igt_collection_create
 * @size: size of the collection, must be greater than 0 and less
 * than #IGT_COLLECTION_MAXSIZE
 *
 * Function creates a collection (set) containing igt_collection_data elements.
 *
 * Returns:
 * pointer to #igt_collection. Asserts on memory allocation failure.
 */
struct igt_collection *igt_collection_create(int size)
{
	struct igt_collection *set;
	int i;

	igt_assert(size > 0 && size <= IGT_COLLECTION_MAXSIZE);

	set = calloc(1, sizeof(*set));
	igt_assert(set);

	set->size = size;
	for (i = 0; i < size; i++)
		set->set[i].value = i; /* set to index as default */

	return set;
}

/**
 * igt_collection_duplicate
 * @src: source collection
 *
 * Function duplicates collection. Useful to cover multithreading
 * when different threads need to get it's own copy of the collection
 * acquired during iteration.
 *
 * Returns:
 * pointer to #igt_collection. Asserts on memory allocation failure.
 */
struct igt_collection *
igt_collection_duplicate(const struct igt_collection *src)
{
	struct igt_collection *set = malloc(sizeof(*set));

	igt_assert(set);
	memcpy(set, src, sizeof(*set));

	return set;
}

/**
 * igt_collection_destroy
 * @set: collection to be freed
 *
 * Function frees collection memory.
 */
void igt_collection_destroy(struct igt_collection *set)
{
	free(set);
}

/**
 * igt_collection_set_value
 * @set: collection
 * @index: index of value data to be set in the collection
 * @value: new value
 *
 * Assign new value to the collection element at @index.
 */
void igt_collection_set_value(struct igt_collection *set, int index, int value)
{
	igt_assert(index >= 0 && index < set->size);
	set->set[index].value = value;
}

/**
 * igt_collection_get_value
 * @set: collection
 * @index: index of value data to be get from the collection
 *
 * Returns: integer value at from the collection element at @index.
 */
int igt_collection_get_value(struct igt_collection *set, int index)
{
	igt_assert(index >= 0 && index < set->size);
	return set->set[index].value;
}

/**
 * igt_collection_set_pointer
 * @set: collection
 * @index: index of pointer data to be set in the collection
 * @ptr: new pointer
 */
void igt_collection_set_pointer(struct igt_collection *set, int index, void *ptr)
{
	igt_assert(index >= 0 && index < set->size);
	set->set[index].ptr = ptr;
}

/**
 * igt_collection_get_pointer
 * @set: collection
 * @index: index of pointer data to be get from the collection
 *
 * Returns: pointer from the collection element at @index.
 */
void *igt_collection_get_pointer(struct igt_collection *set, int index)
{
	igt_assert(index >= 0 && index < set->size);
	return set->set[index].ptr;
}

/**
 * igt_collection_iter_create
 * @set: base collection
 * @result_size: result collection size
 * @algorithm: method of iterating over base collection
 *
 * Function creates iterator which contains result collection changed each time
 * igt_collection_iter_next() is called. For variations without repetitions
 * (VARIATION_R) result collection size can be larger than size of
 * base collection (although still less or equal #IGT_COLLECTION_MAXSIZE).
 * As result collection is a part of the iterator to be thread-safe
 * igt_collection_duplicate() must be called to make result collection copy
 * before passing it to the thread.
 *
 * Returns:
 * pointer to #igt_collection_iter. Asserts on memory allocation failure.
 */
struct igt_collection_iter *
igt_collection_iter_create(const struct igt_collection *set, int result_size,
			   enum igt_collection_iter_algo algorithm)
{
	struct igt_collection_iter *iter;

	igt_assert(result_size > 0 && result_size <= IGT_COLLECTION_MAXSIZE);
	if (algorithm != VARIATION_R)
		igt_assert(result_size <= set->size);

	iter = calloc(1, sizeof(*iter));
	igt_assert(iter);

	iter->set = set;
	iter->result_size = result_size;
	iter->algorithm = algorithm;
	iter->init = true;

	return iter;
}

/**
 * igt_collection_iter_destroy
 * @iter: iterator to be freed
 *
 * Function frees iterator memory.
 */
void igt_collection_iter_destroy(struct igt_collection_iter *iter)
{
	free(iter);
}

static struct igt_collection *
igt_collection_iter_subsets(struct igt_collection_iter *iter)
{
	const struct igt_collection *set = iter->set;
	struct igt_collection *curr = &iter->result;
	int i, pos = 0;

	if (iter->init) {
		iter->init = false;
		iter->data.result_bits = 0;
		iter->data.current_result_size = 0;
		curr->size = 0;
	} else {
		iter->data.result_bits++;
		if (iter->data.result_bits & (1 << iter->set->size)) {
			iter->data.current_result_size++;
			iter->data.result_bits = 0;
		}
		if (iter->data.current_result_size > iter->result_size)
			return NULL;
	}

	while (igt_hweight(iter->data.result_bits) !=
			iter->data.current_result_size) {
		iter->data.result_bits++;
		if (iter->data.result_bits & (1 << iter->set->size)) {
			iter->data.current_result_size++;
			iter->data.result_bits = 0;
		}
	}

	if (iter->data.current_result_size > iter->result_size)
		return NULL;

	for (i = 0; i < set->size; i++) {
		if (!(iter->data.result_bits & (1 << i)))
			continue;
		curr->set[pos++] = set->set[i];
		curr->size = pos;
	}

	return curr;
}

static struct igt_collection *
igt_collection_iter_combination(struct igt_collection_iter *iter)
{
	const struct igt_collection *set = iter->set;
	struct igt_collection *curr = &iter->result;
	int i, pos = 0;

	if (iter->init) {
		iter->init = false;
		iter->data.result_bits = 0;
		iter->result.size = iter->result_size;
	} else {
		iter->data.result_bits++;
	}

	while (igt_hweight(iter->data.result_bits) != iter->result_size)
		iter->data.result_bits++;

	if (iter->data.result_bits & (1 << set->size))
		return NULL;

	for (i = 0; i < set->size; i++) {
		if (!(iter->data.result_bits & (1 << i)))
			continue;
		curr->set[pos++] = set->set[i];
		curr->size = pos;
	}

	return curr;
}

static struct igt_collection *
igt_collection_iter_variation_r(struct igt_collection_iter *iter)
{
	const struct igt_collection *set = iter->set;
	struct igt_collection *curr = &iter->result;
	int i;

	if (iter->init) {
		iter->init = false;
		iter->result.size = iter->result_size;
		for (i = 0; i < iter->result_size; i++)
			iter->data.idxs[i] = 0;
	}

	if (iter->data.idxs[0] == iter->set->size)
		return NULL;

	for (i = 0; i < iter->result_size; i++)
		curr->set[i] = set->set[iter->data.idxs[i]];

	for (i = iter->result_size-1; i >= 0; i--) {
		if (++iter->data.idxs[i] == iter->set->size && i > 0) {
			iter->data.idxs[i] %= iter->set->size;
		} else {
			break;
		}
	}

	return curr;
}

static struct igt_collection *
igt_collection_iter_variation_nr(struct igt_collection_iter *iter)
{
	const struct igt_collection *set = iter->set;
	struct igt_collection *curr = &iter->result;
	bool in_use[IGT_COLLECTION_MAXSIZE];
	bool skip;
	int i;

	if (iter->init) {
		iter->init = false;
		iter->result.size = iter->result_size;
		for (i = 0; i < iter->result_size; i++)
			iter->data.idxs[i] = 0;
	}

	/*
	 * Simple naive algorithm checking does element index is already
	 * occupied.
	 */
retry:
	skip = false;

	if (iter->data.idxs[0] == iter->set->size)
		return NULL;

	for (i = 0; i < iter->result_size; i++)
		curr->set[i] = set->set[iter->data.idxs[i]];

	memset(in_use, 0, sizeof(in_use));
	for (i = 0; i < iter->result_size; i++) {
		if (in_use[iter->data.idxs[i]]) {
			skip = true;
			break;
		}
		in_use[iter->data.idxs[i]] = true;
	}

	for (i = iter->result_size-1; i >= 0; i--) {
		if (++iter->data.idxs[i] == iter->set->size && i > 0) {
			iter->data.idxs[i] %= iter->set->size;
		} else {
			break;
		}
	}

	if (skip)
		goto retry;

	return curr;
}

/**
 * igt_collection_iter_next
 * @iter: collection iterator
 *
 * Function iterates over collection regarding to algorithm selected during
 * iterator creation returning collection (subset or tuples (for variations)).
 *
 * Returns: pointer to the collection (it is a part of the iterator memory
 * so to be thread-safe it must be duplicated by the caller when
 * necessary) or NULL when there're no more elements. Iterator is no longer
 * valid and must be then freed with igt_collection_iter_destroy().
 */
struct igt_collection *
igt_collection_iter_next(struct igt_collection_iter *iter)
{
	struct igt_collection *ret_set = NULL;

	switch(iter->algorithm) {
	case SUBSET:
		ret_set = igt_collection_iter_subsets(iter);
		break;
	case COMBINATION:
		ret_set = igt_collection_iter_combination(iter);
		break;
	case VARIATION_R:
		ret_set = igt_collection_iter_variation_r(iter);
		break;
	case VARIATION_NR:
		ret_set = igt_collection_iter_variation_nr(iter);
		break;
	default:
		igt_assert_f(false, "Unknown algorithm\n");
	}

	return ret_set;
}

/**
 * igt_collection_iter_next_or_end
 * @iter: collection iterator
 *
 * Function does the same as igt_collection_iter_next() but additionally
 * checks when the iterator is no longer valid and frees it then.
 * Useful for for_each_* macros to avoid necessity of manual handling
 * the iterator.
 */
struct igt_collection *
igt_collection_iter_next_or_end(struct igt_collection_iter *iter)
{
	struct igt_collection *ret_set =
			igt_collection_iter_next(iter);

	if (!ret_set)
		igt_collection_iter_destroy(iter);

	return ret_set;
}
