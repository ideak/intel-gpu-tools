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

#include <stdlib.h>
#include <string.h>

#include "igt_core.h"
#include "igt_vec.h"

void igt_vec_init(struct igt_vec *vec, int elem_size)
{
	memset(vec, 0, sizeof(*vec));
	vec->elem_size = elem_size;
}

void igt_vec_fini(struct igt_vec *vec)
{
	free(vec->elems);
	memset(vec, 0, sizeof(*vec));
}

void *igt_vec_elem(const struct igt_vec *vec, int idx)
{
	igt_assert(idx < vec->len);

	return vec->elems + idx * vec->elem_size;
}

static void *igt_vec_grow(struct igt_vec *vec)
{
	if (vec->len++ >= vec->size) {
		vec->size = vec->size ? vec->size * 2 : 8;
		vec->elems = realloc(vec->elems, vec->size * vec->elem_size);
		igt_assert(vec->elems);
	}

	return igt_vec_elem(vec, vec->len - 1);
}

void igt_vec_push(struct igt_vec *vec, void *elem)
{
	memcpy(igt_vec_grow(vec), elem, vec->elem_size);
}

int igt_vec_length(const struct igt_vec *vec)
{
	return vec->len;
}

int igt_vec_index(const struct igt_vec *vec, void *elem)
{
	for (int i = 0; i < vec->len; i++) {
		if (!memcmp(igt_vec_elem(vec, i), elem, vec->elem_size))
			return i;
	}

	return -1;
}

void igt_vec_remove(struct igt_vec *vec, int idx)
{
	igt_assert(idx < vec->len);

	memmove(igt_vec_elem(vec, idx),
		igt_vec_elem(vec, idx + 1),
		(vec->len - 1 - idx) * vec->elem_size);

	vec->len--;
}
