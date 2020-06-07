/*
 * Copyright © 2012 Intel Corporation
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
 *    Ben Widawsky <ben@bwidawsk.net>
 *
 */
#include <limits.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include <drm.h>

#include "i915/gem.h"
#include "igt.h"
#include "igt_dummyload.h"
#include "igt_sysfs.h"
#include "sw_sync.h"

IGT_TEST_DESCRIPTION("Test context batch buffer execution.");

/* Copied from gem_exec_nop.c */
static int exec(int fd, uint32_t handle, int ring, int ctx_id)
{
	struct drm_i915_gem_exec_object2 obj = { .handle = handle };
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = ring,
	};

	i915_execbuffer2_set_context_id(execbuf, ctx_id);

	return __gem_execbuf(fd, &execbuf);
}

static void big_exec(int fd, uint32_t handle, int ring)
{
	int num_buffers = gem_global_aperture_size(fd) / 4096;
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffer_count = num_buffers,
		.flags = ring,
	};
	struct drm_i915_gem_exec_object2 *gem_exec;
	uint32_t ctx_id1, ctx_id2;
	int i;

	/* Make sure we only fill half of RAM with gem objects. */
	igt_require(intel_get_total_ram_mb() * 1024 / 2 > num_buffers * 4);

	gem_exec = calloc(num_buffers + 1, sizeof(*gem_exec));
	igt_assert(gem_exec);
	memset(gem_exec, 0, (num_buffers + 1) * sizeof(*gem_exec));

	ctx_id1 = gem_context_create(fd);
	ctx_id2 = gem_context_create(fd);

	gem_exec[0].handle = handle;

	execbuf.buffers_ptr = to_user_pointer(gem_exec);

	execbuf.buffer_count = 1;
	i915_execbuffer2_set_context_id(execbuf, ctx_id1);
	gem_execbuf(fd, &execbuf);

	for (i = 0; i < num_buffers; i++)
		gem_exec[i].handle = gem_create(fd, 4096);
	gem_exec[i].handle = handle;
	execbuf.buffer_count = i + 1;

	/* figure out how many buffers we can exactly fit */
	while (__gem_execbuf(fd, &execbuf) != 0) {
		i--;
		gem_close(fd, gem_exec[i].handle);
		gem_exec[i].handle = handle;
		execbuf.buffer_count--;
		igt_info("trying buffer count %i\n", i - 1);
	}

	igt_info("reduced buffer count to %i from %i\n", i - 1, num_buffers);

	/* double check that it works */
	gem_execbuf(fd, &execbuf);

	i915_execbuffer2_set_context_id(execbuf, ctx_id2);
	gem_execbuf(fd, &execbuf);
	gem_sync(fd, handle);
}

static void invalid_context(int fd, uint32_t handle)
{
	struct drm_i915_gem_exec_object2 obj = {
		.handle = handle,
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
	};
	unsigned int i;
	uint32_t ctx;

	/* Verify everything works. */
	i915_execbuffer2_set_context_id(execbuf, 0);
	gem_execbuf(fd, &execbuf);

	ctx = gem_context_create(fd);
	i915_execbuffer2_set_context_id(execbuf, ctx);
	igt_assert_eq(__gem_execbuf(fd, &execbuf), 0);

	gem_context_destroy(fd, ctx);

	/* Go through the non-existent context id's. */
	for (i = 0; i < 32; i++) {
		i915_execbuffer2_set_context_id(execbuf, 1UL << i);
		igt_assert_eq(__gem_execbuf(fd, &execbuf), -ENOENT);
	}

	i915_execbuffer2_set_context_id(execbuf, INT_MAX);
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -ENOENT);

	i915_execbuffer2_set_context_id(execbuf, UINT_MAX);
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -ENOENT);
}

static bool has_recoverable_param(int i915)
{
	struct drm_i915_gem_context_param param = {
		.param = I915_CONTEXT_PARAM_RECOVERABLE
	};

	return __gem_context_get_param(i915, &param) == 0;
}

static void norecovery(int i915)
{
	igt_hang_t hang;

	igt_require(has_recoverable_param(i915));
	hang = igt_allow_hang(i915, 0, 0);

	for (int pass = 1; pass >= 0; pass--) {
		struct drm_i915_gem_context_param param = {
			.ctx_id = gem_context_create(i915),
			.param = I915_CONTEXT_PARAM_RECOVERABLE,
			.value = pass,
		};
		int expect = pass == 0 ? -EIO : 0;
		igt_spin_t *spin;

		gem_context_set_param(i915, &param);

		param.value = !pass;
		gem_context_get_param(i915, &param);
		igt_assert_eq(param.value, pass);

		spin = __igt_spin_new(i915,
				      .ctx = param.ctx_id,
				      .flags = IGT_SPIN_POLL_RUN);
		igt_spin_busywait_until_started(spin);

		igt_force_gpu_reset(i915);

		igt_spin_end(spin);
		igt_assert_eq(__gem_execbuf(i915, &spin->execbuf), expect);
		igt_spin_free(i915, spin);

		gem_context_destroy(i915, param.ctx_id);
	}

	 igt_disallow_hang(i915, hang);
}

static bool __enable_hangcheck(int dir, bool state)
{
	return igt_sysfs_set(dir, "enable_hangcheck", state ? "1" : "0");
}

static int __execbuf(int i915, struct drm_i915_gem_execbuffer2 *execbuf)
{
	int err;

	err = 0;
	if (ioctl(i915, DRM_IOCTL_I915_GEM_EXECBUFFER2_WR, execbuf)) {
		err = -errno;
		igt_assume(err);
	}

	errno = 0;
	return err;
}

static void alarm_handler(int sig)
{
}

static int fill_ring(int i915, struct drm_i915_gem_execbuffer2 *execbuf)
{
	struct sigaction old_sa, sa = { .sa_handler = alarm_handler };
	int fence = execbuf->rsvd2 >> 32;
	struct itimerval itv;
	bool once = false;

	sigaction(SIGALRM, &sa, &old_sa);
	itv.it_interval.tv_sec = 0;
	itv.it_interval.tv_usec = 1000;
	itv.it_value.tv_sec = 0;
	itv.it_value.tv_usec = 10000;
	setitimer(ITIMER_REAL, &itv, NULL);

	igt_assert(execbuf->flags & I915_EXEC_FENCE_OUT);
	do {
		int err = __execbuf(i915, execbuf);

		if (err == 0) {
			close(fence);
			fence = execbuf->rsvd2 >> 32;
			continue;
		}

		if (err == -EWOULDBLOCK || once)
			break;

		/* sleep until the next timer interrupt (woken on signal) */
		pause();
		once = true;
	} while (1);

	memset(&itv, 0, sizeof(itv));
	setitimer(ITIMER_REAL, &itv, NULL);
	sigaction(SIGALRM, &old_sa, NULL);

	return fence;
}

static void nohangcheck_hostile(int i915)
{
	const struct intel_execution_engine2 *e;
	igt_hang_t hang;
	int fence = -1;
	uint32_t ctx;
	int err = 0;
	int dir;

	/*
	 * Even if the user disables hangcheck during their context,
	 * we forcibly terminate that context.
	 */

	i915 = gem_reopen_driver(i915);

	dir = igt_params_open(i915);
	igt_require(dir != -1);

	ctx = gem_context_create(i915);
	hang = igt_allow_hang(i915, ctx, 0);

	igt_require(__enable_hangcheck(dir, false));

	____for_each_physical_engine(i915, ctx, e) {
		igt_spin_t *spin;
		int new;

		spin = __igt_spin_new(i915, ctx,
				      .engine = e->flags,
				      .flags = (IGT_SPIN_NO_PREEMPTION |
						IGT_SPIN_FENCE_OUT));

		new = fill_ring(i915, &spin->execbuf);
		igt_assert(new != -1);
		spin->out_fence = -1;

		if (fence < 0) {
			fence = new;
		} else {
			int tmp;

			tmp = sync_fence_merge(fence, new);
			close(fence);
			close(new);

			fence = tmp;
		}
	}
	gem_context_destroy(i915, ctx);
	igt_assert(fence != -1);

	if (sync_fence_wait(fence, MSEC_PER_SEC)) { /* 640ms preempt-timeout */
		igt_debugfs_dump(i915, "i915_engine_info");
		err = -ETIME;
	}

	__enable_hangcheck(dir, true);
	gem_quiescent_gpu(i915);
	igt_disallow_hang(i915, hang);

	igt_assert_f(err == 0,
		     "Hostile unpreemptable context was not cancelled immediately upon closure\n");

	igt_assert_eq(sync_fence_status(fence), -EIO);
	close(fence);

	close(dir);
	close(i915);
}

igt_main
{
	const uint32_t batch[2] = { 0, MI_BATCH_BUFFER_END };
	uint32_t handle;
	uint32_t ctx_id;
	int fd;

	igt_fixture {
		fd = drm_open_driver_render(DRIVER_INTEL);
		igt_require_gem(fd);

		gem_require_contexts(fd);

		handle = gem_create(fd, 4096);
		gem_write(fd, handle, 0, batch, sizeof(batch));
	}

	igt_subtest("basic") {
		ctx_id = gem_context_create(fd);
		igt_assert(exec(fd, handle, 0, ctx_id) == 0);
		gem_sync(fd, handle);
		gem_context_destroy(fd, ctx_id);

		ctx_id = gem_context_create(fd);
		igt_assert(exec(fd, handle, 0, ctx_id) == 0);
		gem_sync(fd, handle);
		gem_context_destroy(fd, ctx_id);

		igt_assert(exec(fd, handle, 0, ctx_id) < 0);
		gem_sync(fd, handle);
	}

	igt_subtest("basic-invalid-context")
		invalid_context(fd, handle);

	igt_subtest("eviction")
		big_exec(fd, handle, 0);

	igt_subtest("basic-norecovery")
		norecovery(fd);

	igt_subtest("basic-nohangcheck")
		nohangcheck_hostile(fd);

	igt_subtest("reset-pin-leak") {
		int i;

		/*
		 * Use an explicit context to isolate the test from
		 * any major code changes related to the per-file
		 * default context (eg. if they would be eliminated).
		 */
		ctx_id = gem_context_create(fd);

		/*
		 * Iterate enough times that the kernel will
		 * become unhappy if the ggtt pin count for
		 * the last context is leaked at every reset.
		 */
		for (i = 0; i < 20; i++) {
			igt_hang_t hang = igt_hang_ring(fd, 0);

			igt_assert_eq(exec(fd, handle, 0, 0), 0);
			igt_assert_eq(exec(fd, handle, 0, ctx_id), 0);
			igt_post_hang_ring(fd, hang);
		}

		gem_context_destroy(fd, ctx_id);
	}
}
