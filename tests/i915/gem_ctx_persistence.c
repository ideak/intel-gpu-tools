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
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "drmtest.h"
#include "i915/gem.h"
#include "i915/gem_context.h"
#include "i915/gem_engine_topology.h"
#include "i915/gem_ring.h"
#include "i915/gem_submission.h"
#include "igt_aux.h"
#include "igt_debugfs.h"
#include "igt_dummyload.h"
#include "igt_gt.h"
#include "igt_sysfs.h"
#include "igt_params.h"
#include "ioctl_wrappers.h" /* gem_wait()! */
#include "sw_sync.h"

#define RESET_TIMEOUT_MS 2 * MSEC_PER_SEC; /* default: 640ms */
static unsigned long reset_timeout_ms = RESET_TIMEOUT_MS;
#define NSEC_PER_MSEC (1000 * 1000ull)

static void cleanup(int i915)
{
	igt_drop_caches_set(i915,
			    /* cancel everything */
			    DROP_RESET_ACTIVE | DROP_RESET_SEQNO |
			    /* cleanup */
			    DROP_ACTIVE | DROP_RETIRE | DROP_IDLE | DROP_FREED);
}

static int wait_for_status(int fence, int timeout)
{
	int err;

	err = sync_fence_wait(fence, timeout);
	if (err)
		return err;

	return sync_fence_status(fence);
}

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

	dir = igt_params_open(i915);
	if (dir < 0) /* no parameters, must be default! */
		return;

	/* If i915.hangcheck is removed, assume the default is good */
	__enable_hangcheck(dir, true);
	close(dir);
}

static void flush_delayed_fput(int i915)
{
	rcu_barrier(i915);
	usleep(50 * 1000);
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

	ctx = gem_context_clone_with_engines(i915, 0);
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

	ctx = gem_context_clone_with_engines(i915, 0);
	gem_context_set_persistence(i915, ctx, false);

	spin = igt_spin_new(i915, ctx,
			    .engine = engine,
			    .flags = IGT_SPIN_FENCE_OUT);
	gem_context_destroy(i915, ctx);

	igt_assert_eq(gem_wait(i915, spin->handle, &timeout), 0);
	igt_assert_eq(sync_fence_status(spin->out_fence), -EIO);

	igt_spin_free(i915, spin);
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

		ctx = gem_context_clone_with_engines(i915, 0);
		gem_context_set_persistence(i915, ctx, i & 1);

		spin = igt_spin_new(i915, ctx,
				    .engine = engine,
				    .flags = IGT_SPIN_FENCE_OUT);
		gem_context_destroy(i915, ctx);

		fence[i] = spin->out_fence;
	}

	/* Outer pair of contexts were non-persistent and killed */
	igt_assert_eq(wait_for_status(fence[0], reset_timeout_ms), -EIO);
	igt_assert_eq(wait_for_status(fence[2], reset_timeout_ms), -EIO);

	/* But the middle context is still running */
	igt_assert_eq(sync_fence_wait(fence[1], 0), -ETIME);
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

	ctx = gem_context_clone_with_engines(i915, 0);
	gem_context_set_persistence(i915, ctx, false);

	spin = igt_spin_new(i915, ctx,
			    .engine = engine,
			    .flags = IGT_SPIN_NO_PREEMPTION);
	gem_context_destroy(i915, ctx);

	igt_assert_eq(gem_wait(i915, spin->handle, &timeout), 0);

	igt_spin_free(i915, spin);
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

	ctx = gem_context_clone_with_engines(i915, 0);
	gem_context_set_persistence(i915, ctx, true);
	gem_context_set_priority(i915, ctx, 0);
	spin[0] = igt_spin_new(i915, ctx,
			       .engine = engine,
			       .flags = (IGT_SPIN_NO_PREEMPTION |
					 IGT_SPIN_POLL_RUN));
	gem_context_destroy(i915, ctx);

	igt_spin_busywait_until_started(spin[0]);

	ctx = gem_context_clone_with_engines(i915, 0);
	gem_context_set_persistence(i915, ctx, false);
	gem_context_set_priority(i915, ctx, 1); /* higher priority than 0 */
	spin[1] = igt_spin_new(i915, ctx,
			       .engine = engine,
			       .flags = IGT_SPIN_NO_PREEMPTION);
	gem_context_destroy(i915, ctx);

	igt_assert_eq(gem_wait(i915, spin[1]->handle, &timeout), 0);

	igt_spin_free(i915, spin[1]);
	igt_spin_free(i915, spin[0]);
}

static void test_nonpersistent_hang(int i915, unsigned int engine)
{
	int64_t timeout = reset_timeout_ms * NSEC_PER_MSEC;
	igt_spin_t *spin;
	uint32_t ctx;

	/*
	 * The user made a simple mistake and submitted an invalid batch,
	 * but fortunately under a nonpersistent context. Do we detect it?
	 */

	ctx = gem_context_create(i915);
	gem_context_set_persistence(i915, ctx, false);

	spin = igt_spin_new(i915, ctx,
			    .engine = engine,
			    .flags = IGT_SPIN_INVALID_CS);
	gem_context_destroy(i915, ctx);

	igt_assert_eq(gem_wait(i915, spin->handle, &timeout), 0);

	igt_spin_free(i915, spin);
}

static void test_nohangcheck_hostile(int i915)
{
	int64_t timeout = reset_timeout_ms * NSEC_PER_MSEC;
	int dir;

	cleanup(i915);

	/*
	 * Even if the user disables hangcheck during their context,
	 * we forcibly terminate that context.
	 */

	dir = igt_params_open(i915);
	igt_require(dir != -1);

	igt_require(__enable_hangcheck(dir, false));

	for_each_engine(e, i915) {
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
	close(dir);
}

static void test_nohangcheck_hang(int i915)
{
	int64_t timeout = reset_timeout_ms * NSEC_PER_MSEC;
	int dir;

	cleanup(i915);

	/*
	 * Even if the user disables hangcheck during their context,
	 * we forcibly terminate that context.
	 */

	igt_require(!gem_has_cmdparser(i915, ALL_ENGINES));

	dir = igt_params_open(i915);
	igt_require(dir != -1);

	igt_require(__enable_hangcheck(dir, false));

	for_each_engine(e, i915) {
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
	close(dir);
}

static void test_nonpersistent_file(int i915)
{
	int debugfs = i915;
	igt_spin_t *spin;

	cleanup(i915);

	/*
	 * A context may live beyond its initial struct file, except if it
	 * has been made nonpersistent, in which case it must be terminated.
	 */

	i915 = gem_reopen_driver(i915);

	gem_context_set_persistence(i915, 0, false);
	spin = igt_spin_new(i915, .flags = IGT_SPIN_FENCE_OUT);

	close(i915);
	flush_delayed_fput(debugfs);

	igt_assert_eq(wait_for_status(spin->out_fence, reset_timeout_ms), -EIO);

	spin->handle = 0;
	igt_spin_free(-1, spin);
}

static int __execbuf_wr(int i915, struct drm_i915_gem_execbuffer2 *execbuf)
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

static void test_nonpersistent_queued(int i915, unsigned int engine)
{
	struct sigaction old_sa, sa = { .sa_handler = alarm_handler };
	struct itimerval itv;
	igt_spin_t *spin;
	int fence = -1;
	uint32_t ctx;

	/*
	 * Not only must the immediate batch be cancelled, but
	 * all pending batches in the context.
	 */

	ctx = gem_context_clone_with_engines(i915, 0);
	gem_context_set_persistence(i915, ctx, false);
	spin = igt_spin_new(i915, ctx,
			    .engine = engine,
			    .flags = IGT_SPIN_FENCE_OUT);

	sigaction(SIGALRM, &sa, &old_sa);
	memset(&itv, 0, sizeof(itv));
	itv.it_value.tv_sec = 1;
	itv.it_value.tv_usec = 0;
	setitimer(ITIMER_REAL, &itv, NULL);

	fcntl(i915, F_SETFL, fcntl(i915, F_GETFL) | O_NONBLOCK);
	while (1) {
		igt_assert(spin->execbuf.flags & I915_EXEC_FENCE_OUT);
		if (__execbuf_wr(i915, &spin->execbuf))
			break;

		if (fence != -1)
			close(fence);

		igt_assert(spin->execbuf.rsvd2);
		fence = spin->execbuf.rsvd2 >> 32;
	}
	fcntl(i915, F_SETFL, fcntl(i915, F_GETFL) & ~O_NONBLOCK);

	memset(&itv, 0, sizeof(itv));
	setitimer(ITIMER_REAL, &itv, NULL);
	sigaction(SIGALRM, &old_sa, NULL);

	gem_context_destroy(i915, ctx);

	igt_assert_eq(wait_for_status(spin->out_fence, reset_timeout_ms), -EIO);
	igt_assert_eq(wait_for_status(fence, reset_timeout_ms), -EIO);

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

	cleanup(i915);

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

	igt_assert_eq(wait_for_status(fence, reset_timeout_ms), -EIO);
	close(fence);

	/* We have to manually clean up the orphaned spinner */
	igt_drop_caches_set(i915, DROP_RESET_ACTIVE);

	gem_quiescent_gpu(i915);
}

static void test_process_mixed(int pfd, unsigned int engine)
{
	int fence[2], sv[2];

	/*
	 * If a process dies early, any nonpersistent contexts it had
	 * open must be terminated too. But any persistent contexts,
	 * should survive until their requests are complete.
	 */

	igt_require(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);

	igt_fork(child, 1) {
		int i915;

		i915 = gem_reopen_driver(pfd);
		gem_quiescent_gpu(i915);

		for (int persists = 0; persists <= 1; persists++) {
			igt_spin_t *spin;
			uint32_t ctx;

			ctx = gem_context_create(i915);
			gem_context_copy_engines(pfd, 0, i915, ctx);
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
	flush_delayed_fput(pfd);

	fence[0] = recvfd(sv[1]);
	fence[1] = recvfd(sv[1]);
	close(sv[1]);

	/* First fence is non-persistent, so should be reset */
	igt_assert_eq(wait_for_status(fence[0], reset_timeout_ms), -EIO);
	close(fence[0]);

	/* Second fence is persistent, so should be still spinning */
	igt_assert_eq(sync_fence_wait(fence[1], 0), -ETIME);
	close(fence[1]);

	/* We have to manually clean up the orphaned spinner */
	igt_drop_caches_set(pfd, DROP_RESET_ACTIVE);

	gem_quiescent_gpu(pfd);
}

static void
test_saturated_hostile(int i915, const struct intel_execution_engine2 *engine)
{
	const struct intel_execution_engine2 *other;
	igt_spin_t *spin;
	uint32_t ctx;
	int fence = -1;

	cleanup(i915);

	/*
	 * Check that if we have to remove a hostile request from a
	 * non-persistent context, we do so without harming any other
	 * concurrent users.
	 *
	 * We only allow non-persistent contexts if we can perform a
	 * per-engine reset, that is removal of the hostile context without
	 * impacting other users on the system. [Consider the problem of
	 * allowing the user to create a context with which they can arbitrarily
	 * reset other users whenever they chose.]
	 */

	__for_each_physical_engine(i915, other) {
		if (other->flags == engine->flags)
			continue;

		spin = igt_spin_new(i915,
				   .engine = other->flags,
				   .flags = (IGT_SPIN_NO_PREEMPTION |
					     IGT_SPIN_FENCE_OUT));

		if (fence < 0) {
			fence = spin->out_fence;
		} else {
			int tmp;

			tmp = sync_fence_merge(fence, spin->out_fence);
			close(fence);
			close(spin->out_fence);

			fence = tmp;
		}
		spin->out_fence = -1;
	}
	igt_require(fence != -1);

	ctx = gem_context_clone_with_engines(i915, 0);
	gem_context_set_persistence(i915, ctx, false);
	spin = igt_spin_new(i915, ctx,
			    .engine = engine->flags,
			    .flags = (IGT_SPIN_NO_PREEMPTION |
				      IGT_SPIN_POLL_RUN |
				      IGT_SPIN_FENCE_OUT));
	igt_spin_busywait_until_started(spin);
	gem_context_destroy(i915, ctx);

	/* Hostile request requires a GPU reset to terminate */
	igt_assert_eq(wait_for_status(spin->out_fence, reset_timeout_ms), -EIO);

	/* All other spinners should be left unharmed */
	gem_quiescent_gpu(i915);
	igt_assert_eq(wait_for_status(fence, reset_timeout_ms), 1);
	close(fence);
}

static void test_processes(int i915)
{
	struct {
		int sv[2];
	} p[2];

	cleanup(i915);

	/*
	 * If one process dies early, its nonpersistent context are cleaned up,
	 * but that should not affect a second process.
	 */

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
			igt_assert_eq(wait_for_status(fence, reset_timeout_ms),
				      -EIO);
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

static void __smoker(int i915,
		     unsigned int engine,
		     unsigned int timeout,
		     int expected)
{
	igt_spin_t *spin;
	int fence = -1;
	int fd, extra;

	fd = gem_reopen_driver(i915);
	gem_context_copy_engines(i915, 0, fd, 0);
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

	igt_assert_eq(wait_for_status(spin->out_fence, timeout), expected);

	if (fence != -1) {
		igt_assert_eq(wait_for_status(fence, timeout), expected);
		close(fence);
	}

	spin->handle = 0;
	igt_spin_free(fd, spin);
}

static void smoker(int i915,
		   unsigned int engine,
		   unsigned int timeout,
		   unsigned int *ctl)
{
	while (!READ_ONCE(*ctl)) {
		__smoker(i915, engine, timeout, -EIO);
		__smoker(i915, engine, timeout, 1);
	}
}

static void smoketest(int i915)
{
	const int SMOKE_LOAD_FACTOR = 4;
	const struct intel_execution_engine2 *e;
	uint32_t *ctl;

	cleanup(i915);

	/*
	 * All of the above! A mixture of naive and hostile processes and
	 * contexts, all trying to trick the kernel into mass slaughter.
	 */

	ctl = mmap(0, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(ctl != MAP_FAILED);

	for (int i = 1; i <= SMOKE_LOAD_FACTOR; i <<= 1) {
		*ctl = 0;

		igt_debug("Applying load factor: %d\n", i);
		__for_each_physical_engine(i915, e) {
			igt_fork(child, i)
				smoker(i915,
				       e->flags,
				       i * reset_timeout_ms,
				       ctl);
		}

		sleep(10);
		*ctl = 1;
		igt_waitchildren();
	}

	munmap(ctl, 4096);
	gem_quiescent_gpu(i915);
}

static void replace_engines(int i915, const struct intel_execution_engine2 *e)
{
	I915_DEFINE_CONTEXT_PARAM_ENGINES(engines, 1) = {
		.engines = {{ e->class, e->instance }}
	};
	struct drm_i915_gem_context_param param = {
		.ctx_id = gem_context_create(i915),
		.param = I915_CONTEXT_PARAM_ENGINES,
		.value = to_user_pointer(&engines),
		.size = sizeof(engines),
	};
	igt_spin_t *spin[2];
	int64_t timeout;

	/*
	 * Suppose the user tries to hide a hanging batch by replacing
	 * the set of engines on the context so that it's not visible
	 * at the time of closure? Then we must act when they replace
	 * the engines!
	 */

	gem_context_set_persistence(i915, param.ctx_id, false);

	gem_context_set_param(i915, &param);
	spin[0] = igt_spin_new(i915, param.ctx_id);

	gem_context_set_param(i915, &param);
	spin[1] = igt_spin_new(i915, param.ctx_id);

	gem_context_destroy(i915, param.ctx_id);

	timeout = reset_timeout_ms * NSEC_PER_MSEC;
	igt_assert_eq(gem_wait(i915, spin[1]->handle, &timeout), 0);

	timeout = reset_timeout_ms * NSEC_PER_MSEC;
	igt_assert_eq(gem_wait(i915, spin[0]->handle, &timeout), 0);

	igt_spin_free(i915, spin[1]);
	igt_spin_free(i915, spin[0]);
	gem_quiescent_gpu(i915);
}

static void race_set_engines(int i915, int in, int out)
{
	I915_DEFINE_CONTEXT_PARAM_ENGINES(engines, 1) = {
		.engines = {}
	};
	struct drm_i915_gem_context_param param = {
		.param = I915_CONTEXT_PARAM_ENGINES,
		.value = to_user_pointer(&engines),
		.size = sizeof(engines),
	};
	igt_spin_t *spin;

	spin = igt_spin_new(i915);
	igt_spin_end(spin);

	while (read(in, &param.ctx_id, sizeof(param.ctx_id)) > 0) {
		if (!param.ctx_id)
			break;

		__gem_context_set_param(i915, &param);

		spin->execbuf.rsvd1 = param.ctx_id;
		__gem_execbuf(i915, &spin->execbuf);

		write(out, &param.ctx_id, sizeof(param.ctx_id));
	}

	igt_spin_free(i915, spin);
}

static void close_replace_race(int i915)
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	int fence = -1;
	int out[2], in[2];

	cleanup(i915);

	/*
	 * If we time the submission of a hanging batch to one set of engines
	 * and then simultaneously replace the engines in one thread, and
	 * close the context in another, it might be possible for the kernel
	 * to lose track of the old engines believing that the non-persisten
	 * context is already closed and the hanging requests cancelled.
	 *
	 * Our challenge is try and expose any such race condition.
	 */

	igt_assert(pipe(out) == 0);
	igt_assert(pipe(in) == 0);
	igt_fork(child, ncpus) {
		close(out[1]);
		close(in[0]);
		race_set_engines(i915, out[0], in[1]);
	}
	for (int i = 0; i < ncpus; i++)
		close(out[0]);

	igt_until_timeout(5) {
		igt_spin_t *spin;
		uint32_t ctx;

		ctx = gem_context_clone_with_engines(i915, 0);
		gem_context_set_persistence(i915, ctx, false);

		spin = igt_spin_new(i915, ctx, .flags = IGT_SPIN_FENCE_OUT);
		for (int i = 0; i < ncpus; i++)
			write(out[1], &ctx, sizeof(ctx));

		gem_context_destroy(i915, ctx);
		for (int i = 0; i < ncpus; i++)
			read(in[0], &ctx, sizeof(ctx));

		if (fence < 0) {
			fence = spin->out_fence;
		} else {
			int tmp;

			tmp = sync_fence_merge(fence, spin->out_fence);
			close(fence);
			close(spin->out_fence);

			fence = tmp;
		}
		spin->out_fence = -1;
	}
	close(in[0]);

	for (int i = 0; i < ncpus; i++) {
		uint32_t end = 0;

		write(out[1], &end, sizeof(end));
	}
	close(out[1]);

	if (sync_fence_wait(fence, MSEC_PER_SEC / 2)) {
		igt_debugfs_dump(i915, "i915_engine_info");
		igt_assert(sync_fence_wait(fence, MSEC_PER_SEC / 2) == 0);
	}
	close(fence);

	igt_waitchildren();
	gem_quiescent_gpu(i915);
}

static void replace_engines_hostile(int i915,
				    const struct intel_execution_engine2 *e)
{
	I915_DEFINE_CONTEXT_PARAM_ENGINES(engines, 1) = {
		.engines = {{ e->class, e->instance }}
	};
	struct drm_i915_gem_context_param param = {
		.ctx_id = gem_context_create(i915),
		.param = I915_CONTEXT_PARAM_ENGINES,
		.value = to_user_pointer(&engines),
		.size = sizeof(engines),
	};
	int64_t timeout = reset_timeout_ms * NSEC_PER_MSEC;
	igt_spin_t *spin;

	/*
	 * Suppose the user tries to hide a hanging batch by replacing
	 * the set of engines on the context so that it's not visible
	 * at the time of closure? Then we must act when they replace
	 * the engines!
	 */

	gem_context_set_persistence(i915, param.ctx_id, false);

	gem_context_set_param(i915, &param);
	spin = igt_spin_new(i915, param.ctx_id,
			    .flags = IGT_SPIN_NO_PREEMPTION);

	param.size = 8;
	gem_context_set_param(i915, &param);
	gem_context_destroy(i915, param.ctx_id);

	igt_assert_eq(gem_wait(i915, spin->handle, &timeout), 0);

	igt_spin_free(i915, spin);
	gem_quiescent_gpu(i915);
}

static void do_test(void (*test)(int i915, unsigned int engine),
		    int i915, unsigned int engine,
		    const char *name)
{
#define ATTR "preempt_timeout_ms"
	int timeout = -1;

	cleanup(i915);

	gem_engine_property_scanf(i915, name, ATTR, "%d", &timeout);
	if (timeout != -1) {
		igt_require(gem_engine_property_printf(i915, name,
						       ATTR, "%d", 50) > 0);
		reset_timeout_ms = 200;
	}

	test(i915, engine);

	if (timeout != -1) {
		gem_engine_property_printf(i915, name, ATTR, "%d", timeout);
		reset_timeout_ms = RESET_TIMEOUT_MS;
	}

	gem_quiescent_gpu(i915);
}

int i915;

static void exit_handler(int sig)
{
	enable_hangcheck(i915);
}

igt_main
{
	struct {
		const char *name;
		void (*func)(int fd, unsigned int engine);
	} *test, tests[] = {
		{ "persistence", test_persistence },
		{ "cleanup", test_nonpersistent_cleanup },
		{ "queued", test_nonpersistent_queued },
		{ "mixed", test_nonpersistent_mixed },
		{ "mixed-process", test_process_mixed },
		{ "hostile", test_nonpersistent_hostile },
		{ "hostile-preempt", test_nonpersistent_hostile_preempt },
		{ "hang", test_nonpersistent_hang },
		{ NULL, NULL },
	};

	igt_fixture {
		i915 = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(i915);

		/* Restore the reset modparam if left clobbered */
		igt_assert(igt_params_set(i915, "reset", "%d", -1));

		enable_hangcheck(i915);
		igt_install_exit_handler(exit_handler);

		igt_require(has_persistence(i915));
		igt_allow_hang(i915, 0, 0);
	}

	/* Legacy execbuf engine selection flags. */

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

	igt_subtest_group {
		igt_fixture
			gem_require_contexts(i915);

		for (test = tests; test->name; test++) {
			igt_subtest_with_dynamic_f("legacy-engines-%s",
						   test->name) {
				for_each_physical_engine(e, i915) {
					igt_dynamic_f("%s", e->name) {
						do_test(test->func,
							i915, eb_ring(e),
							e->full_name);
					}
				}
			}
		}

		/* Assert things are under control. */
		igt_assert(!gem_context_has_engine_map(i915, 0));
	}

	/* New way of selecting engines. */

	igt_subtest_group {
		const struct intel_execution_engine2 *e;

		igt_fixture
			gem_require_contexts(i915);

		for (test = tests; test->name; test++) {
			igt_subtest_with_dynamic_f("engines-%s", test->name) {
				__for_each_physical_engine(i915, e) {
					igt_dynamic_f("%s", e->name) {
						do_test(test->func,
							i915, e->flags,
							e->name);
					}
				}
			}
		}

		igt_subtest_with_dynamic_f("saturated-hostile") {
			__for_each_physical_engine(i915, e) {
				igt_dynamic_f("%s", e->name)
					test_saturated_hostile(i915, e);
			}
		}

		igt_subtest("smoketest")
			smoketest(i915);
	}

	/* Check interactions with set-engines */
	igt_subtest_group {
		const struct intel_execution_engine2 *e;

		igt_fixture
			gem_require_contexts(i915);

		igt_subtest_with_dynamic("replace") {
			__for_each_physical_engine(i915, e) {
				igt_dynamic_f("%s", e->name)
					replace_engines(i915, e);
			}
		}

		igt_subtest_with_dynamic("replace-hostile") {
			__for_each_physical_engine(i915, e) {
				igt_dynamic_f("%s", e->name)
					replace_engines_hostile(i915, e);
			}
		}

		igt_subtest("close-replace-race")
			close_replace_race(i915);
	}

	igt_fixture {
		close(i915);
	}
}
