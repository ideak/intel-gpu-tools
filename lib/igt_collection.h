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

#ifndef __IGT_COLLECTION_H__
#define __IGT_COLLECTION_H__

#include <stdbool.h>

/* Maximum collection size we support, don't change unless you understand
 * the implementation */
#define IGT_COLLECTION_MAXSIZE 16

enum igt_collection_iter_algo {
	SUBSET,
	COMBINATION,
	VARIATION_R,  /* variations with repetition */
	VARIATION_NR, /* variations without repetitions */
};

struct igt_collection_data {
	int value;
	void *ptr;
};

struct igt_collection {
	int size;
	struct igt_collection_data set[IGT_COLLECTION_MAXSIZE];
};

struct igt_collection_iter;

struct igt_collection *igt_collection_create(int size);
struct igt_collection *igt_collection_duplicate(const struct igt_collection *src);

void igt_collection_destroy(struct igt_collection *set);
void igt_collection_set_value(struct igt_collection *set, int index, int value);
int igt_collection_get_value(struct igt_collection *set, int index);
void igt_collection_set_pointer(struct igt_collection *set, int index, void *ptr);
void *igt_collection_get_pointer(struct igt_collection *set, int index);

struct igt_collection_iter *
igt_collection_iter_create(const struct igt_collection *set, int subset_size,
			   enum igt_collection_iter_algo algorithm);

void igt_collection_iter_destroy(struct igt_collection_iter *iter);
struct igt_collection *igt_collection_iter_next(struct igt_collection_iter *iter);
struct igt_collection *igt_collection_iter_next_or_end(struct igt_collection_iter *iter);

#define for_each_subset(__result, __size, __set) \
	for (struct igt_collection_iter *igt_tokencat(__it, __LINE__) = \
		igt_collection_iter_create(__set, __size, SUBSET); \
		((__result) = igt_collection_iter_next_or_end(\
			igt_tokencat(__it, __LINE__))); )

#define for_each_combination(__result, __size, __set) \
	for (struct igt_collection_iter *igt_tokencat(__it, __LINE__) = \
		igt_collection_iter_create(__set, __size, COMBINATION); \
		((__result) = igt_collection_iter_next_or_end(\
			igt_tokencat(__it, __LINE__))); )

#define for_each_variation_r(__result, __size, __set) \
	for (struct igt_collection_iter *igt_tokencat(__it, __LINE__) = \
		igt_collection_iter_create(__set, __size, VARIATION_R); \
		((__result) = igt_collection_iter_next_or_end(\
			igt_tokencat(__it, __LINE__))); )

#define for_each_variation_nr(__result, __size, __set) \
	for (struct igt_collection_iter *igt_tokencat(__it, __LINE__) = \
		igt_collection_iter_create(__set, __size, VARIATION_NR); \
		((__result) = igt_collection_iter_next_or_end(\
			igt_tokencat(__it, __LINE__))); )

#define for_each_collection_data(__data, __set) \
	for (int igt_tokencat(__i, __LINE__) = 0; \
		(__data = (igt_tokencat(__i, __LINE__) < __set->size) ? \
		 &__set->set[igt_tokencat(__i, __LINE__)] : NULL); \
		igt_tokencat(__i, __LINE__)++)

#endif /* __IGT_COLLECTION_H__ */
