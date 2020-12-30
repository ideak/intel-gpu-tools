/*
 * Copyright © 2014 Intel Corporation
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

#ifndef GEM_RING_H
#define GEM_RING_H

#include <stdbool.h>

extern const struct intel_execution_ring {
	const char *name;
	const char *full_name;
	unsigned exec_id;
	unsigned flags;
} intel_execution_rings[];

#define eb_ring(e) ((e)->exec_id | (e)->flags)

#define for_each_ring(it__, fd__) \
	for (const struct intel_execution_ring *it__ = intel_execution_rings;\
	     it__->name; \
	     it__++) \
		for_if (gem_has_ring(fd__, eb_ring(it__)))

#define for_each_physical_ring(it__, fd__) \
	for (const struct intel_execution_ring *it__ = intel_execution_rings;\
	     it__->name; \
	     it__++) \
		for_if (gem_ring_has_physical_engine(fd__, eb_ring(it__)))

bool gem_ring_is_physical_engine(int fd, unsigned int ring);
bool gem_ring_has_physical_engine(int fd, unsigned int ring);

enum measure_ring_flags {
	MEASURE_RING_NEW_CTX = 1
};

unsigned int
gem_measure_ring_inflight(int fd, unsigned int engine, enum measure_ring_flags flags);

#endif /* GEM_RING_H */
