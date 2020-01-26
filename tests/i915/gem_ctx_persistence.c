/*
 * Copyright © 2019 Intel Corporation
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

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "drmtest.h" /* gem_quiescent_gpu()! */
#include "i915/gem_context.h"
#include "i915/gem_engine_topology.h"
#include "i915/gem_ring.h"
#include "i915/gem_submission.h"
#include "igt_debugfs.h"
#include "igt_dummyload.h"
#include "igt_gt.h"
#include "igt_sysfs.h"
#include "ioctl_wrappers.h" /* gem_wait()! */
#include "sw_sync.h"

static unsigned long reset_timeout_ms = MSEC_PER_SEC; /* default: 640ms */
#define NSEC_PER_MSEC (1000 * 1000ull)

static bool has_persistence(int i915)
{
	struct drm_i915_gem_context_param p = {
		.param = I915_CONTEXT_PARAM_PERSISTENCE,
	};
	uint64_t saved;

	if (__gem_context_get_param(i915, &p))
		return false;

	saved = p.value;
	p.value = 0;
	if (__gem_context_set_param(i915, &p))
		return false;

	p.value = saved;
	return __gem_context_set_param(i915, &p) == 0;
}

static bool __enable_hangcheck(int dir, bool state)
{
	return igt_sysfs_set(dir, "enable_hangcheck", state ? "1" : "0");
}

static void enable_hangcheck(int i915)
{
	int dir;

	dir = igt_sysfs_open_parameters(i915);
	if (dir < 0) /* no parameters, must be default! */
		return;

	/* If i915.hangcheck is removed, assume the default is good */
	__enable_hangcheck(dir, true);
	close(dir);
}

static void flush_delayed_fput(int i915)
{
	rcu_barrier(i915); /* flush the delayed fput */
	sched_yield();
	rcu_barrier(i915); /* again, in case it was added after we waited! */
}

static void test_idempotent(int i915)
{
	struct drm_i915_gem_context_param p = {
		.param = I915_CONTEXT_PARAM_PERSISTENCE,
	};
	int expected;

	/*
	 * Simple test to verify that we are able to read back the same boolean
	 * value as we set.
	 *
	 * Each time we invert the current value so that at the end of the test,
	 * if successful, we leave the context in the original state.
	 */

	gem_context_get_param(i915, &p);
	expected = !!p.value;

	expected = !expected;
	p.value = expected;
	gem_context_set_param(i915, &p);
	gem_context_get_param(i915, &p);
	igt_assert_eq(p.value, expected);

	expected = !expected; /* and restores */
	p.value = expected;
	gem_context_set_param(i915, &p);
	gem_context_get_param(i915, &p);
	igt_assert_eq(p.value, expected);
}

static void test_clone(int i915)
{
	struct drm_i915_gem_context_param p = {
		.param = I915_CONTEXT_PARAM_PERSISTENCE,
	};
	uint32_t ctx, clone;

	/*
	 * Check that persistence is inherited across a clone.
	 */
	igt_require( __gem_context_create(i915, &ctx) == 0);

	p.ctx_id = ctx;
	p.value = 0;
	gem_context_set_param(i915, &p);

	clone = gem_context_clone(i915, ctx, I915_CONTEXT_CLONE_FLAGS, 0);
	gem_context_destroy(i915, ctx);

	p.ctx_id = clone;
	p.value = -1;
	gem_context_get_param(i915, &p);
	igt_assert_eq(p.value, 0);

	gem_context_destroy(i915, clone);
}

static void test_persistence(int i915, unsigned int engine)
{
	igt_spin_t *spin;
	int64_t timeout;
	uint32_t ctx;

	/*
	 * Default behaviour are contexts remain alive until their last active
	 * request is retired -- no early termination.
	 */

	ctx = gem_context_create(i915);
	gem_context_set_persistence(i915, ctx, true);

	spin = igt_spin_new(i915, ctx,
			    .engine = engine,
			    .flags = IGT_SPIN_FENCE_OUT);
	gem_context_destroy(i915, ctx);

	timeout = reset_timeout_ms * NSEC_PER_MSEC;
	igt_assert_eq(gem_wait(i915, spin->handle, &timeout), -ETIME);

	igt_spin_end(spin);

	timeout = reset_timeout_ms * NSEC_PER_MSEC;
	igt_assert_eq(gem_wait(i915, spin->handle, &timeout), 0);
	igt_assert_eq(sync_fence_status(spin->out_fence), 1);

	igt_spin_free(i915, spin);
	gem_quiescent_gpu(i915);
}

static void test_nonpersistent_cleanup(int i915, unsigned int engine)
{
	int64_t timeout = reset_timeout_ms * NSEC_PER_MSEC;
	igt_spin_t *spin;
	uint32_t ctx;

	/*
	 * A nonpersistent context is terminated immediately upon closure,
	 * any inflight request is cancelled.
	 */

	ctx = gem_context_create(i915);
	gem_context_set_persistence(i915, ctx, false);

	spin = igt_spin_new(i915, ctx,
			    .engine = engine,
			    .flags = IGT_SPIN_FENCE_OUT);
	gem_context_destroy(i915, ctx);

	igt_assert_eq(gem_wait(i915, spin->handle, &timeout), 0);
	igt_assert_eq(sync_fence_status(spin->out_fence), -EIO);

	igt_spin_free(i915, spin);
	gem_quiescent_gpu(i915);
}

static void test_nonpersistent_mixed(int i915, unsigned int engine)
{
	int fence[3];

	/*
	 * Only a nonpersistent context is terminated immediately upon
	 * closure, any inflight request is cancelled. If there is also
	 * an active persistent context closed, it should be unafffected.
	 */

	for (int i = 0; i < ARRAY_SIZE(fence); i++) {
		igt_spin_t *spin;
		uint32_t ctx;

		ctx = gem_context_create(i915);
		gem_context_set_persistence(i915, ctx, i & 1);

		spin = igt_spin_new(i915, ctx,
				    .engine = engine,
				    .flags = IGT_SPIN_FENCE_OUT);
		gem_context_destroy(i915, ctx);

		fence[i] = spin->out_fence;
	}

	/* Outer pair of contexts were non-persistent and killed */
	igt_assert_eq(sync_fence_wait(fence[0], reset_timeout_ms), 0);
	igt_assert_eq(sync_fence_status(fence[0]), -EIO);

	igt_assert_eq(sync_fence_wait(fence[2], reset_timeout_ms), 0);
	igt_assert_eq(sync_fence_status(fence[2]), -EIO);

	/* But the middle context is still running */
	igt_assert_eq(sync_fence_wait(fence[1], 0), -ETIME);

	gem_quiescent_gpu(i915);
}

static void test_nonpersistent_hostile(int i915, unsigned int engine)
{
	int64_t timeout = reset_timeout_ms * NSEC_PER_MSEC;
	igt_spin_t *spin;
	uint32_t ctx;

	/*
	 * If we cannot cleanly cancel the non-persistent context on closure,
	 * e.g. preemption fails, we are forced to reset the GPU to terminate
	 * the requests and cleanup the context.
	 */

	ctx = gem_context_create(i915);
	gem_context_set_persistence(i915, ctx, false);

	spin = igt_spin_new(i915, ctx,
			    .engine = engine,
			    .flags = IGT_SPIN_NO_PREEMPTION);
	gem_context_destroy(i915, ctx);

	igt_assert_eq(gem_wait(i915, spin->handle, &timeout), 0);

	igt_spin_free(i915, spin);
	gem_quiescent_gpu(i915);
}

static void test_nonpersistent_hostile_preempt(int i915, unsigned int engine)
{
	int64_t timeout = reset_timeout_ms * NSEC_PER_MSEC;
	igt_spin_t *spin[2];
	uint32_t ctx;

	/*
	 * Double plus ungood.
	 *
	 * Here we would not be able to cancel the hostile non-persistent
	 * context and we cannot preempt-to-idle as it is already waiting
	 * on preemption for itself. Let's hope the kernel can save the
	 * day with a reset.
	 */

	igt_require(gem_scheduler_has_preemption(i915));

	ctx = gem_context_create(i915);
	gem_context_set_persistence(i915, ctx, true);
	gem_context_set_priority(i915, ctx, 0);
	spin[0] = igt_spin_new(i915, ctx,
			       .engine = engine,
			       .flags = (IGT_SPIN_NO_PREEMPTION |
					 IGT_SPIN_POLL_RUN));
	gem_context_destroy(i915, ctx);

	igt_spin_busywait_until_started(spin[0]);

	ctx = gem_context_create(i915);
	gem_context_set_persistence(i915, ctx, false);
	gem_context_set_priority(i915, ctx, 1); /* higher priority than 0 */
	spin[1] = igt_spin_new(i915, ctx,
			       .engine = engine,
			       .flags = IGT_SPIN_NO_PREEMPTION);
	gem_context_destroy(i915, ctx);

	igt_assert_eq(gem_wait(i915, spin[1]->handle, &timeout), 0);

	igt_spin_free(i915, spin[1]);
	igt_spin_free(i915, spin[0]);
	gem_quiescent_gpu(i915);
}

static void test_nohangcheck_hostile(int i915)
{
	int64_t timeout = reset_timeout_ms * NSEC_PER_MSEC;
	int dir;

	/*
	 * Even if the user disables hangcheck during their context,
	 * we forcibly terminate that context.
	 */

	dir = igt_sysfs_open_parameters(i915);
	igt_require(dir != -1);

	igt_require(__enable_hangcheck(dir, false));

	for_each_physical_engine(e, i915) {
		uint32_t ctx = gem_context_create(i915);
		igt_spin_t *spin;

		spin = igt_spin_new(i915, ctx,
				    .engine = eb_ring(e),
				    .flags = IGT_SPIN_NO_PREEMPTION);
		gem_context_destroy(i915, ctx);

		igt_assert_eq(gem_wait(i915, spin->handle, &timeout), 0);

		igt_spin_free(i915, spin);
	}

	igt_require(__enable_hangcheck(dir, true));

	gem_quiescent_gpu(i915);
	close(dir);
}

static void test_nohangcheck_hang(int i915)
{
	int64_t timeout = reset_timeout_ms * NSEC_PER_MSEC;
	int dir;

	/*
	 * Even if the user disables hangcheck during their context,
	 * we forcibly terminate that context.
	 */

	dir = igt_sysfs_open_parameters(i915);
	igt_require(dir != -1);

	igt_require(__enable_hangcheck(dir, false));

	for_each_physical_engine(e, i915) {
		uint32_t ctx = gem_context_create(i915);
		igt_spin_t *spin;

		spin = igt_spin_new(i915, ctx,
				    .engine = eb_ring(e),
				    .flags = IGT_SPIN_INVALID_CS);
		gem_context_destroy(i915, ctx);

		igt_assert_eq(gem_wait(i915, spin->handle, &timeout), 0);

		igt_spin_free(i915, spin);
	}

	igt_require(__enable_hangcheck(dir, true));

	gem_quiescent_gpu(i915);
	close(dir);
}

static void test_nonpersistent_file(int i915)
{
	int debugfs = i915;
	igt_spin_t *spin;

	/*
	 * A context may live beyond its initial struct file, except if it
	 * has been made nonpersistent, in which case it must be terminated.
	 */

	i915 = gem_reopen_driver(i915);
	gem_quiescent_gpu(i915);

	gem_context_set_persistence(i915, 0, false);
	spin = igt_spin_new(i915, .flags = IGT_SPIN_FENCE_OUT);

	close(i915);
	flush_delayed_fput(debugfs);

	igt_assert_eq(sync_fence_wait(spin->out_fence, reset_timeout_ms), 0);
	igt_assert_eq(sync_fence_status(spin->out_fence), -EIO);

	spin->handle = 0;
	igt_spin_free(-1, spin);
}

static void test_nonpersistent_queued(int i915, unsigned int engine)
{
	const int count = gem_measure_ring_inflight(i915, engine, 0);
	igt_spin_t *spin;
	int fence = -1;
	uint32_t ctx;

	/*
	 * Not only must the immediate batch be cancelled, but
	 * all pending batches in the context.
	 */

	gem_quiescent_gpu(i915);

	ctx = gem_context_create(i915);
	gem_context_set_persistence(i915, ctx, false);
	spin = igt_spin_new(i915, ctx,
			    .engine = engine,
			    .flags = IGT_SPIN_FENCE_OUT);

	for (int i = 0; i < count - 1; i++) {
		spin->execbuf.rsvd2 = 0;
		if (fence != -1)
			close(fence);

		igt_assert(spin->execbuf.flags & I915_EXEC_FENCE_OUT);
		gem_execbuf_wr(i915, &spin->execbuf);

		igt_assert(spin->execbuf.rsvd2);
		fence = spin->execbuf.rsvd2 >> 32;
	}

	gem_context_destroy(i915, ctx);

	igt_assert_eq(sync_fence_wait(spin->out_fence, reset_timeout_ms), 0);
	igt_assert_eq(sync_fence_status(spin->out_fence), -EIO);

	igt_assert_eq(sync_fence_wait(fence, reset_timeout_ms), 0);
	igt_assert_eq(sync_fence_status(fence), -EIO);

	igt_spin_free(i915, spin);
}

static void sendfd(int socket, int fd)
{
	char buf[CMSG_SPACE(sizeof(fd))];
	struct iovec io = { .iov_base = (char *)"ABC", .iov_len = 3 };
	struct msghdr msg = {
		.msg_iov = &io,
		.msg_iovlen = 1,
		.msg_control = buf,
		.msg_controllen = CMSG_LEN(sizeof(fd)),
	};
	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = msg.msg_controllen;
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));

	igt_assert(sendmsg(socket, &msg, 0) != -1);
}

static int recvfd(int socket)
{
	char m_buffer[256], c_buffer[256];
	struct iovec io = {
		.iov_base = m_buffer,
		.iov_len = sizeof(m_buffer),
	};
	struct msghdr msg = {
		.msg_iov = &io,
		.msg_iovlen = 1,
		.msg_control = c_buffer,
		.msg_controllen = sizeof(c_buffer),
	};

	igt_assert(recvmsg(socket, &msg, 0) != -1);
	return *(int *)CMSG_DATA(CMSG_FIRSTHDR(&msg));
}

static void test_process(int i915)
{
	int fence, sv[2];

	/*
	 * If a process dies early, any nonpersistent contexts it had
	 * open must be terminated too.
	 */

	igt_require(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);

	igt_fork(child, 1) {
		igt_spin_t *spin;

		i915 = gem_reopen_driver(i915);
		gem_quiescent_gpu(i915);

		gem_context_set_persistence(i915, 0, false);
		spin = igt_spin_new(i915, .flags = IGT_SPIN_FENCE_OUT);
		sendfd(sv[0], spin->out_fence);

		igt_list_del(&spin->link); /* prevent autocleanup */
	}
	close(sv[0]);
	igt_waitchildren();
	flush_delayed_fput(i915);

	fence = recvfd(sv[1]);
	close(sv[1]);

	igt_assert_eq(sync_fence_wait(fence, reset_timeout_ms), 0);
	igt_assert_eq(sync_fence_status(fence), -EIO);
	close(fence);

	/* We have to manually clean up the orphaned spinner */
	igt_drop_caches_set(i915, DROP_RESET_ACTIVE);

	gem_quiescent_gpu(i915);
}

static void test_process_mixed(int i915, unsigned int engine)
{
	int fence[2], sv[2];

	/*
	 * If a process dies early, any nonpersistent contexts it had
	 * open must be terminated too. But any persistent contexts,
	 * should survive until their requests are complete.
	 */

	igt_require(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);

	igt_fork(child, 1) {
		i915 = gem_reopen_driver(i915);
		gem_quiescent_gpu(i915);

		for (int persists = 0; persists <= 1; persists++) {
			igt_spin_t *spin;
			uint32_t ctx;

			ctx = gem_context_create(i915);
			gem_context_set_persistence(i915, ctx, persists);

			spin = igt_spin_new(i915, ctx,
					    .engine = engine,
					    .flags = IGT_SPIN_FENCE_OUT);

			sendfd(sv[0], spin->out_fence);

			igt_list_del(&spin->link); /* prevent autocleanup */
		}
	}
	close(sv[0]);
	igt_waitchildren();
	flush_delayed_fput(i915);

	fence[0] = recvfd(sv[1]);
	fence[1] = recvfd(sv[1]);
	close(sv[1]);

	/* First fence is non-persistent, so should be reset */
	igt_assert_eq(sync_fence_wait(fence[0], reset_timeout_ms), 0);
	igt_assert_eq(sync_fence_status(fence[0]), -EIO);
	close(fence[0]);

	/* Second fence is persistent, so should be still spinning */
	igt_assert_eq(sync_fence_wait(fence[1], 0), -ETIME);
	close(fence[1]);

	/* We have to manually clean up the orphaned spinner */
	igt_drop_caches_set(i915, DROP_RESET_ACTIVE);

	gem_quiescent_gpu(i915);
}

static void test_processes(int i915)
{
	struct {
		int sv[2];
	} p[2];

	/*
	 * If one process dies early, its nonpersistent context are cleaned up,
	 * but that should not affect a second process.
	 */

	gem_quiescent_gpu(i915);
	for (int i = 0; i < ARRAY_SIZE(p); i++) {
		igt_require(socketpair(AF_UNIX, SOCK_DGRAM, 0, p[i].sv) == 0);

		igt_fork(child, 1) {
			igt_spin_t *spin;
			int pid;

			i915 = gem_reopen_driver(i915);
			gem_context_set_persistence(i915, 0, i);

			spin = igt_spin_new(i915, .flags = IGT_SPIN_FENCE_OUT);
			/* prevent autocleanup */
			igt_list_del(&spin->link);

			sendfd(p[i].sv[0], spin->out_fence);

			/* Wait until we are told to die */
			pid = getpid();
			write(p[i].sv[0], &pid, sizeof(pid));

			pid = 0;
			read(p[i].sv[0], &pid, sizeof(pid));
			igt_assert(pid == getpid());
		}
	}

	for (int i = 0; i < ARRAY_SIZE(p); i++) {
		int fence, pid;

		/* The process is not dead yet, so the context can spin. */
		fence = recvfd(p[i].sv[1]);
		igt_assert_eq(sync_fence_wait(fence, 0), -ETIME);

		/* Kill *this* process */
		read(p[i].sv[1], &pid, sizeof(pid));
		write(p[i].sv[1], &pid, sizeof(pid));

		/*
		 * A little bit of slack required for the signal to terminate
		 * the process and for the system to cleanup the fd.
		 */
		sched_yield();
		close(p[i].sv[0]);
		close(p[i].sv[1]);
		flush_delayed_fput(i915);

		if (i == 0) {
			/* First fence is non-persistent, so should be reset */
			igt_assert_eq(sync_fence_wait(fence,
						      reset_timeout_ms), 0);
			igt_assert_eq(sync_fence_status(fence), -EIO);
		} else {
			/* Second fence is persistent, so still spinning */
			igt_assert_eq(sync_fence_wait(fence, 0), -ETIME);
		}
		close(fence);
	}
	igt_waitchildren();

	/* We have to manually clean up the orphaned spinner */
	igt_drop_caches_set(i915, DROP_RESET_ACTIVE);
	gem_quiescent_gpu(i915);
}

static void __smoker(int i915, unsigned int engine, int expected)
{
	igt_spin_t *spin;
	int fence = -1;
	int fd, extra;

	fd = gem_reopen_driver(i915);
	gem_context_set_persistence(fd, 0, expected > 0);
	spin = igt_spin_new(fd, .engine = engine, .flags = IGT_SPIN_FENCE_OUT);

	extra = rand() % 8;
	while (extra--) {
		if (fence != -1)
			close(fence);
		spin->execbuf.rsvd2 = 0;
		gem_execbuf_wr(fd, &spin->execbuf);
		igt_assert(spin->execbuf.rsvd2);
		fence = spin->execbuf.rsvd2 >> 32;
	}

	close(fd);
	flush_delayed_fput(i915);

	igt_spin_end(spin);

	igt_assert_eq(sync_fence_wait(spin->out_fence, reset_timeout_ms), 0);
	igt_assert_eq(sync_fence_status(spin->out_fence), expected);

	if (fence != -1) {
		igt_assert_eq(sync_fence_wait(fence, reset_timeout_ms), 0);
		igt_assert_eq(sync_fence_status(fence), expected);
		close(fence);
	}

	spin->handle = 0;
	igt_spin_free(i915, spin);
}

static void smoker(int i915, unsigned int engine, unsigned int *ctl)
{
	while (!READ_ONCE(*ctl)) {
		__smoker(i915, engine, -EIO);
		__smoker(i915, engine, 1);
	}
}

static void smoketest(int i915)
{
	uint32_t *ctl;

	/*
	 * All of the above! A mixture of naive and hostile processes and
	 * contexts, all trying to trick the kernel into mass slaughter.
	 */

	ctl = mmap(0, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(ctl != MAP_FAILED);

	for_each_physical_engine(e, i915) {
		igt_fork(child, 4)
			smoker(i915, eb_ring(e), ctl);
	}

	sleep(20);
	*ctl = 1;
	igt_waitchildren();

	munmap(ctl, 4096);
	gem_quiescent_gpu(i915);
}

igt_main
{
	const struct intel_execution_engine2 *e;
	int i915;

	igt_fixture {
		i915 = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(i915);

		igt_require(has_persistence(i915));
		enable_hangcheck(i915);

		igt_allow_hang(i915, 0, 0);
	}

	igt_subtest("idempotent")
		test_idempotent(i915);

	igt_subtest("clone")
		test_clone(i915);

	igt_subtest("file")
		test_nonpersistent_file(i915);

	igt_subtest("process")
		test_process(i915);

	igt_subtest("processes")
		test_processes(i915);

	igt_subtest("hostile")
		test_nohangcheck_hostile(i915);
	igt_subtest("hang")
		test_nohangcheck_hang(i915);

	__for_each_static_engine(e) {
		igt_subtest_group {
			igt_fixture {
				gem_require_ring(i915, e->flags);
				gem_require_contexts(i915);
			}

			igt_subtest_f("%s-persistence", e->name)
				test_persistence(i915, e->flags);

			igt_subtest_f("%s-cleanup", e->name)
				test_nonpersistent_cleanup(i915, e->flags);

			igt_subtest_f("%s-queued", e->name)
				test_nonpersistent_queued(i915, e->flags);

			igt_subtest_f("%s-mixed", e->name)
				test_nonpersistent_mixed(i915, e->flags);

			igt_subtest_f("%s-mixed-process", e->name)
				test_process_mixed(i915, e->flags);

			igt_subtest_f("%s-hostile", e->name)
				test_nonpersistent_hostile(i915, e->flags);

			igt_subtest_f("%s-hostile-preempt", e->name)
				test_nonpersistent_hostile_preempt(i915,
								   e->flags);
		}
	}

	igt_subtest("smoketest")
		smoketest(i915);

	igt_fixture {
		close(i915);
	}
}
