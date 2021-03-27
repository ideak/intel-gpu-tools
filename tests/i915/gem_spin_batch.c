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
 *
 */

#include "i915/gem.h"
#include "i915/gem_ring.h"
#include "igt.h"

#define MAX_ERROR 5 /* % */

#define assert_within_epsilon(x, ref, tolerance) \
	igt_assert_f(100 * x <= (100 + tolerance) * ref && \
		     100 * x >= (100 - tolerance) * ref, \
		     "'%s' != '%s' (%lld not within %d%% tolerance of %lld)\n",\
		     #x, #ref, (long long)x, tolerance, (long long)ref)

static void spin(int fd, const intel_ctx_t *ctx_id,
		 unsigned int engine,
		 unsigned int flags,
		 unsigned int timeout_sec)
{
	const uint64_t timeout_100ms = 100000000LL;
	unsigned long loops = 0;
	igt_spin_t *spin;
	struct timespec tv = { };
	struct timespec itv = { };
	uint64_t elapsed;

	spin = __igt_spin_new(fd, .ctx = ctx_id, .engine = engine,
			      .flags = flags);
	while ((elapsed = igt_nsec_elapsed(&tv)) >> 30 < timeout_sec) {
		igt_spin_t *next =
			__igt_spin_new(fd, .ctx = ctx_id, .engine = engine,
				       .flags = flags);

		igt_spin_set_timeout(spin,
				     timeout_100ms - igt_nsec_elapsed(&itv));
		gem_sync(fd, spin->handle);
		igt_debug("loop %lu: interval=%fms (target 100ms), elapsed %fms\n",
			  loops,
			  igt_nsec_elapsed(&itv) * 1e-6,
			  igt_nsec_elapsed(&tv) * 1e-6);
		igt_nsec_elapsed(memset(&itv, 0, sizeof(itv)));

		igt_spin_free(fd, spin);
		spin = next;
		loops++;
	}
	igt_spin_free(fd, spin);

	igt_info("Completed %ld loops in %lld ns, target %ld\n",
		 loops, (long long)elapsed, (long)(elapsed / timeout_100ms));

	assert_within_epsilon(timeout_100ms * loops, elapsed, MAX_ERROR);
}

#define RESUBMIT_NEW_CTX     (1 << 0)
#define RESUBMIT_ALL_ENGINES (1 << 1)

static void spin_resubmit(int fd, const intel_ctx_t *ctx,
			  unsigned int engine, unsigned int flags)
{
	const intel_ctx_t *new_ctx = NULL;
	igt_spin_t *spin;

	if (flags & RESUBMIT_NEW_CTX)
		igt_require(gem_has_contexts(fd));

	spin = __igt_spin_new(fd, .ctx = ctx, .engine = engine);
	if (flags & RESUBMIT_NEW_CTX) {
		new_ctx = intel_ctx_create(fd, &ctx->cfg);
		spin->execbuf.rsvd1 = new_ctx->id;
	}

	if (flags & RESUBMIT_ALL_ENGINES) {
		const struct intel_execution_engine2 *other;

		for_each_ctx_engine(fd, ctx, other) {
			spin->execbuf.flags &= ~0x3f;
			spin->execbuf.flags |= other->flags;
			gem_execbuf(fd, &spin->execbuf);
		}
	}

	gem_execbuf(fd, &spin->execbuf);
	igt_spin_end(spin);
	gem_sync(fd, spin->handle);

	if (flags & RESUBMIT_NEW_CTX)
		intel_ctx_destroy(fd, new_ctx);

	igt_spin_free(fd, spin);
}

static void spin_exit_handler(int sig)
{
	igt_terminate_spins();
}

static void
spin_on_all_engines(int fd, const intel_ctx_t *ctx,
		    unsigned long flags, unsigned int timeout_sec)
{
	const struct intel_execution_engine2 *e2;

	for_each_ctx_engine(fd, ctx, e2) {
		igt_fork(child, 1) {
			igt_install_exit_handler(spin_exit_handler);
			spin(fd, ctx, e2->flags, flags, timeout_sec);
		}
	}

	igt_waitchildren();
}

static void spin_all(int i915, const intel_ctx_t *ctx, unsigned int flags)
#define PARALLEL_SPIN_NEW_CTX BIT(0)
{
	const struct intel_execution_engine2 *e;
	intel_ctx_cfg_t cfg = ctx->cfg;
	struct igt_spin *spin, *n;
	IGT_LIST_HEAD(list);

	for_each_ctx_cfg_engine(i915, &cfg, e) {
		if (!gem_class_can_store_dword(i915, e->class))
			continue;

		if (flags & PARALLEL_SPIN_NEW_CTX)
			ctx = intel_ctx_create(i915, &cfg);

		/* Prevent preemption so only one is allowed on each engine */
		spin = igt_spin_new(i915,
				    .ctx = ctx,
				    .engine = e->flags,
				    .flags = (IGT_SPIN_POLL_RUN |
					      IGT_SPIN_NO_PREEMPTION));
		if (flags & PARALLEL_SPIN_NEW_CTX)
			intel_ctx_destroy(i915, ctx);

		igt_spin_busywait_until_started(spin);
		igt_list_move(&spin->link, &list);
	}

	igt_list_for_each_entry_safe(spin, n, &list, link) {
		igt_assert(gem_bo_busy(i915, spin->handle));
		igt_spin_end(spin);
		gem_sync(i915, spin->handle);
		igt_spin_free(i915, spin);
	}
}

static bool has_userptr(int fd)
{
	struct drm_i915_gem_userptr userptr;
	int err;

	memset(&userptr, 0, sizeof(userptr));
	userptr.user_size = 8192;
	userptr.user_ptr = -4096;

	err = 0;
	if (drmIoctl(fd, DRM_IOCTL_I915_GEM_USERPTR, &userptr)) {
		err = errno;
		igt_assume(err);
	}
	errno = 0;

	return err == EFAULT;
}

igt_main
{
	const struct intel_execution_engine2 *e2;
	const struct intel_execution_ring *e;
	const intel_ctx_t *ctx;
	int fd = -1;

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);
		ctx = intel_ctx_create_all_physical(fd);

		igt_fork_hang_detector(fd);
	}

#define test_each_legacy_ring(test) \
	igt_subtest_with_dynamic(test) \
		for (e = intel_execution_rings; e->name; e++) \
			if (gem_has_ring(fd, eb_ring(e))) \
				igt_dynamic_f("%s", e->name)

	test_each_legacy_ring("legacy")
		spin(fd, intel_ctx_0(fd), eb_ring(e), 0, 3);
	test_each_legacy_ring("legacy-resubmit")
		spin_resubmit(fd, intel_ctx_0(fd), eb_ring(e), 0);
	test_each_legacy_ring("legacy-resubmit-new")
		spin_resubmit(fd, intel_ctx_0(fd), eb_ring(e), RESUBMIT_NEW_CTX);

#undef test_each_legcy_ring

	igt_subtest("spin-all")
		spin_all(fd, ctx, 0);
	igt_subtest("spin-all-new")
		spin_all(fd, ctx, PARALLEL_SPIN_NEW_CTX);

#define test_each_engine(test) \
	igt_subtest_with_dynamic(test) \
		for_each_ctx_engine(fd, ctx, e2) \
			igt_dynamic_f("%s", e2->name)

	test_each_engine("engines")
		spin(fd, ctx, e2->flags, 0, 3);

	test_each_engine("resubmit")
		spin_resubmit(fd, ctx, e2->flags, 0);

	test_each_engine("resubmit-new")
		spin_resubmit(fd, ctx, e2->flags,
			      RESUBMIT_NEW_CTX);

	test_each_engine("resubmit-all")
		spin_resubmit(fd, ctx, e2->flags,
			      RESUBMIT_ALL_ENGINES);

	test_each_engine("resubmit-new-all")
		spin_resubmit(fd, ctx, e2->flags,
			      RESUBMIT_NEW_CTX |
			      RESUBMIT_ALL_ENGINES);

#undef test_each_engine

	igt_subtest("spin-each")
		spin_on_all_engines(fd, ctx, 0, 3);

	igt_subtest("user-each") {
		igt_require(has_userptr(fd));
		spin_on_all_engines(fd, ctx, IGT_SPIN_USERPTR, 3);
	}

	igt_fixture {
		igt_stop_hang_detector();
		intel_ctx_destroy(fd, ctx);
		close(fd);
	}
}
