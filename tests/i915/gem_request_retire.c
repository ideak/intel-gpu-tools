/*
 * Copyright © 2015 Intel Corporation
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
 *    Tvrtko Ursulin <tvrtko.ursulin@intel.com>
 *
 */

/** @file gem_request_retire
 *
 * Collection of tests targeting request retirement code paths.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>

#include "drm.h"
#include "i915_drm.h"

#include "i915/gem.h"
#include "igt.h"

IGT_TEST_DESCRIPTION("Collection of tests targeting request retirement code"
		     " paths.");

/*
 * A single bo is operated from batchbuffers submitted from two contexts and on
 * different rings.
 * One execbuf finishes way ahead of the other at which point the respective
 * context is destroyed.
 */
static void
test_retire_vma_not_inactive(int fd)
{
	struct intel_execution_engine2 *e;
	const intel_ctx_t *ctx;
	igt_spin_t *bg = NULL;

	ctx = intel_ctx_create_all_physical(fd);

	for_each_ctx_engine(fd, ctx, e) {
		igt_spin_t *spin;
		const intel_ctx_t *spin_ctx;

		if (!bg) {
			bg = igt_spin_new(fd, .ctx = ctx, .engine = e->flags);
			continue;
		}

		spin_ctx = intel_ctx_create(fd, &ctx->cfg);
		spin = igt_spin_new(fd, .ctx = spin_ctx,
				    .engine = e->flags,
				    .dependency = bg->handle,
				    .flags = IGT_SPIN_SOFTDEP);
		intel_ctx_destroy(fd, spin_ctx);
		igt_spin_end(spin);

		gem_sync(fd, spin->handle);
		igt_spin_free(fd, spin);
	}

	igt_drop_caches_set(fd, DROP_RETIRE);
	igt_spin_free(fd, bg);
	intel_ctx_destroy(fd, ctx);
}

int fd;

igt_main
{
	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);
		gem_require_blitter(fd);

		gem_require_contexts(fd);
	}

	igt_subtest("retire-vma-not-inactive")
		test_retire_vma_not_inactive(fd);
}
