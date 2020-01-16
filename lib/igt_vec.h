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

#ifndef __IGT_VEC_H__
#define __IGT_VEC_H__

struct igt_vec {
	void *elems;
	int elem_size, size, len;
};

void igt_vec_init(struct igt_vec *vec, int elem_size);
void igt_vec_fini(struct igt_vec *vec);
void igt_vec_push(struct igt_vec *vec, void *elem);
int igt_vec_length(const struct igt_vec *vec);
void *igt_vec_elem(const struct igt_vec *vec, int idx);
int igt_vec_index(const struct igt_vec *vec, void *elem);
void igt_vec_remove(struct igt_vec *vec, int idx);

#endif /* __IGT_VEC_H__ */
