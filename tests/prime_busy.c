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
 */

#include <sys/poll.h>

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt.h"

IGT_TEST_DESCRIPTION("Basic check of polling for prime fences.");

static bool prime_busy(struct pollfd *pfd, bool excl)
{
	pfd->events = excl ? POLLOUT : POLLIN;
	return poll(pfd, 1, 0) == 0;
}

#define BEFORE 0x1
#define AFTER 0x2
#define HANG 0x4
#define POLL 0x8

static void busy(int fd, const intel_ctx_t *ctx, unsigned ring, unsigned flags)
{
#define SCRATCH 0
#define BATCH 1
	uint32_t handle = gem_create(fd, 4096);
	struct pollfd pfd[2] = {};
	uint64_t ahnd;
	igt_spin_t *spin;
	int timeout;

	gem_quiescent_gpu(fd);

	ahnd = get_reloc_ahnd(fd, ctx->id);
	spin = igt_spin_new(fd,
			    .ahnd = ahnd,
			    .ctx = ctx, .engine = ring,
			    .dependency = handle,
			    .flags = (flags & HANG ? IGT_SPIN_NO_PREEMPTION : 0));
	igt_spin_end(spin);
	gem_sync(fd, spin->handle);

	if (flags & BEFORE) {
		pfd[SCRATCH].fd = prime_handle_to_fd(fd, spin->obj[SCRATCH].handle);
		pfd[BATCH].fd = prime_handle_to_fd(fd, spin->obj[BATCH].handle);
	}

	igt_spin_reset(spin);
	gem_execbuf(fd, &spin->execbuf);

	if (flags & AFTER) {
		pfd[SCRATCH].fd = prime_handle_to_fd(fd, spin->obj[SCRATCH].handle);
		pfd[BATCH].fd = prime_handle_to_fd(fd, spin->obj[BATCH].handle);
	}

	igt_assert(prime_busy(&pfd[SCRATCH], false));
	igt_assert(prime_busy(&pfd[SCRATCH], true));

	igt_assert(!prime_busy(&pfd[BATCH], false));
	igt_assert(prime_busy(&pfd[BATCH], true));

	timeout = 120;
	if ((flags & HANG) == 0) {
		igt_spin_end(spin);
		timeout = 1;
	}

	/* Calling busy in a loop should be enough to flush the rendering */
	if (flags & POLL) {
		pfd[BATCH].events = POLLOUT;
		igt_assert(poll(pfd, 1, timeout * 1000) == 1);
	} else {
		struct timespec tv = {};
		while (prime_busy(&pfd[BATCH], true))
			igt_assert(igt_seconds_elapsed(&tv) < timeout);
	}
	igt_assert(!prime_busy(&pfd[SCRATCH], true));

	igt_spin_free(fd, spin);
	gem_close(fd, handle);

	close(pfd[BATCH].fd);
	close(pfd[SCRATCH].fd);
	put_ahnd(ahnd);
}

static void test_mode(int fd, const intel_ctx_t *ctx, unsigned int flags)
{
	const struct intel_execution_engine2 *e;
	igt_hang_t hang = {};

	if ((flags & HANG) == 0)
		igt_fork_hang_detector(fd);
	else
		hang = igt_allow_hang(fd, ctx->id, 0);

	for_each_ctx_engine(fd, ctx, e) {
		igt_dynamic_f("%s", e->name)
			busy(fd, ctx, e->flags, flags);
	}

	if ((flags & HANG) == 0)
		igt_stop_hang_detector();
	else
		igt_disallow_hang(fd, hang);
}

igt_main
{
	const intel_ctx_t *ctx;
	int fd = -1;

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);
		ctx = intel_ctx_create_all_physical(fd);
	}

	igt_subtest_group {
		const struct mode {
			const char *name;
			unsigned int flags;
		} modes[] = {
			{ "before", BEFORE },
			{ "after", AFTER },
			{ "hang", BEFORE | HANG },
			{ },
		};

		igt_fixture
			gem_require_mmap_device_coherent(fd);

		for (const struct mode *m = modes; m->name; m++) {
			igt_subtest_with_dynamic(m->name)
				test_mode(fd, ctx, m->flags);

			igt_subtest_with_dynamic_f("%s-wait", m->name)
				test_mode(fd, ctx, m->flags | POLL);
		}
	}

	igt_fixture {
		intel_ctx_destroy(fd, ctx);
		close(fd);
	}
}
