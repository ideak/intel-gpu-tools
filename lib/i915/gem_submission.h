/*
 * Copyright © 2017 Intel Corporation
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

#ifndef GEM_SUBMISSION_H
#define GEM_SUBMISSION_H

#include <stdint.h>

#include "intel_ctx.h"

#define GEM_SUBMISSION_SEMAPHORES	(1 << 0)
#define GEM_SUBMISSION_EXECLISTS	(1 << 1)
#define GEM_SUBMISSION_GUC		(1 << 2)
unsigned gem_submission_method(int fd);
void gem_submission_print_method(int fd);
bool gem_has_semaphores(int fd);
bool gem_has_execlists(int fd);
bool gem_has_guc_submission(int fd);
bool gem_engine_has_mutable_submission(int fd, unsigned int engine);
bool gem_class_has_mutable_submission(int fd, int class);

int gem_cmdparser_version(int i915);
static inline bool gem_has_cmdparser(int i915)
{
	return gem_cmdparser_version(i915) > 0;
}
bool gem_engine_has_cmdparser(int i915, const intel_ctx_cfg_t *cfg,
			      unsigned int engine);

bool gem_has_blitter(int i915);
void gem_require_blitter(int i915);

unsigned int gem_submission_measure(int i915, const intel_ctx_cfg_t *cfg,
				    unsigned int engine);

void gem_test_all_engines(int fd);
bool gem_has_relocations(int fd);

#endif /* GEM_SUBMISSION_H */
