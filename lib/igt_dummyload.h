/*
 * Copyright © 2016 Intel Corporation
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
 */

#ifndef __IGT_DUMMYLOAD_H__
#define __IGT_DUMMYLOAD_H__

#include <stdint.h>
#include <time.h>

#include "igt_aux.h"

typedef struct igt_spin {
	unsigned int handle;
	timer_t timer;
	struct igt_list link;
	uint32_t *batch;
} igt_spin_t;

igt_spin_t *igt_spin_batch_new(int fd, int engine, unsigned int dep_handle);
void igt_spin_batch_set_timeout(igt_spin_t *spin, int64_t ns);
void igt_spin_batch_end(igt_spin_t *spin);
void igt_spin_batch_free(int fd, igt_spin_t *spin);

void igt_terminate_spin_batches(void);

#endif /* __IGT_DUMMYLOAD_H__ */
