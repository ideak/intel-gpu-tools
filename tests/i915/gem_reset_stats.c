/*
 * Copyright (c) 2013 Intel Corporation
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
 *  Mika Kuoppala <mika.kuoppala@intel.com>
 *
 */

#include <limits.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <signal.h>
#include <poll.h>

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/gem_ring.h"
#include "igt.h"
#include "igt_sysfs.h"
#include "sw_sync.h"

#define RS_NO_ERROR      0
#define RS_BATCH_ACTIVE  (1 << 0)
#define RS_BATCH_PENDING (1 << 1)
#define RS_UNKNOWN       (1 << 2)


static uint32_t devid;

struct local_drm_i915_reset_stats {
	__u32 ctx_id;
	__u32 flags;
	__u32 reset_count;
	__u32 batch_active;
	__u32 batch_pending;
	__u32 pad;
};

struct spin_ctx {
	unsigned int class;
	unsigned int instance;
	const intel_ctx_t *ctx;
	int ahnd;
	igt_spin_t *spin;
};

#define MAX_FD 32

#define GET_RESET_STATS_IOCTL DRM_IOWR(DRM_COMMAND_BASE + 0x32, struct local_drm_i915_reset_stats)

static int device;

static bool __enable_hangcheck(int dir, bool state)
{
	return igt_sysfs_set(dir, "enable_hangcheck", state ? "1" : "0");
}

static void enable_hangcheck(int i915, bool state)
{
	int dir;

	dir = igt_params_open(i915);
	if (dir < 0) /* no parameters, must be default! */
		return;

	__enable_hangcheck(dir, state);
	close(dir);
}

static void set_unbannable(int i915, uint32_t ctx)
{
	struct drm_i915_gem_context_param p = {
		.ctx_id = ctx,
		.param = I915_CONTEXT_PARAM_BANNABLE,
	};

	gem_context_set_param(i915, &p);
}

static void
create_spinner(int i915,  const intel_ctx_cfg_t *base_cfg, struct spin_ctx *_spin,
		int engine_flag, int prio, unsigned int flags)
{
	_spin->ctx = intel_ctx_create(i915, base_cfg);
	set_unbannable(i915, _spin->ctx->id);
	gem_context_set_priority(i915, _spin->ctx->id, prio);
	_spin->ahnd = get_reloc_ahnd(i915, _spin->ctx->id);

	_spin->spin = igt_spin_new(i915, .ahnd = _spin->ahnd,
			.ctx = _spin->ctx, .engine = engine_flag, .flags = flags);
	igt_spin_busywait_until_started(_spin->spin);
}

static void sync_gpu(void)
{
	gem_quiescent_gpu(device);
}

static int noop(int fd, uint32_t ctx, const struct intel_execution_ring *e)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_execbuffer2 eb;
	struct drm_i915_gem_exec_object2 exec;
	int ret;

	memset(&exec, 0, sizeof(exec));
	exec.handle = gem_create(fd, 4096);
	igt_assert((int)exec.handle > 0);
	gem_write(fd, exec.handle, 0, &bbe, sizeof(bbe));

	memset(&eb, 0, sizeof(eb));
	eb.buffers_ptr = to_user_pointer(&exec);
	eb.buffer_count = 1;
	eb.flags = eb_ring(e);
	i915_execbuffer2_set_context_id(eb, ctx);

	ret = __gem_execbuf(fd, &eb);
	if (ret < 0) {
		gem_close(fd, exec.handle);
		return ret;
	}

	return exec.handle;
}

static int has_engine(int fd,
		      uint32_t ctx,
		      const struct intel_execution_ring *e)
{
	int handle = noop(fd, ctx, e);
	if (handle < 0)
		return 0;
	gem_close(fd, handle);
	return 1;
}

static void check_context(const struct intel_execution_ring *e)
{
	gem_require_contexts(device);
	igt_require(has_engine(device, gem_context_create(device), e));
}

static int gem_reset_stats(int fd, int ctx_id,
			   struct local_drm_i915_reset_stats *rs)
{
	memset(rs, 0, sizeof(*rs));
	rs->ctx_id = ctx_id;
	rs->reset_count = -1;

	if (drmIoctl(fd, GET_RESET_STATS_IOCTL, rs))
		return -errno;

	igt_assert(rs->reset_count != -1);
	return 0;
}

static int gem_reset_status(int fd, int ctx_id)
{
	struct local_drm_i915_reset_stats rs;
	int ret;

	ret = gem_reset_stats(fd, ctx_id, &rs);
	if (ret)
		return ret;

	if (rs.batch_active)
		return RS_BATCH_ACTIVE;
	if (rs.batch_pending)
		return RS_BATCH_PENDING;

	return RS_NO_ERROR;
}

static struct timespec ts_injected;

#define BAN HANG_ALLOW_BAN
#define ASYNC 2
static void inject_hang(int fd, uint32_t ctx,
			const struct intel_execution_ring *e,
			unsigned flags)
{
	igt_hang_t hang;

	clock_gettime(CLOCK_MONOTONIC, &ts_injected);

	hang = igt_hang_ctx(fd, ctx, eb_ring(e), flags & BAN);
	if ((flags & ASYNC) == 0)
		igt_post_hang_ring(fd, hang);
}

static const char *status_to_string(int x)
{
	const char *strings[] = {
		"No error",
		"Guilty",
		"Pending",
	};
	if (x >= ARRAY_SIZE(strings))
		return "Unknown";
	return strings[x];
}

static int _assert_reset_status(int idx, int fd, int ctx, int status)
{
	int rs;

	rs = gem_reset_status(fd, ctx);
	if (rs < 0) {
		igt_info("reset status for %d ctx %d returned %d\n",
			 idx, ctx, rs);
		return rs;
	}

	if (rs != status) {
		igt_info("%d:%d expected '%s' [%d], found '%s' [%d]\n",
			 idx, ctx,
			 status_to_string(status), status,
			 status_to_string(rs), rs);

		return 1;
	}

	return 0;
}

#define assert_reset_status(idx, fd, ctx, status) \
	igt_assert(_assert_reset_status(idx, fd, ctx, status) == 0)

static void test_rs(const struct intel_execution_ring *e,
		    int num_fds, int hang_index, int rs_assumed_no_hang)
{
	int fd[MAX_FD];
	int i;

	igt_assert_lte(num_fds, MAX_FD);
	igt_assert_lt(hang_index, MAX_FD);

	igt_debug("num fds=%d, hang index=%d\n", num_fds, hang_index);

	for (i = 0; i < num_fds; i++) {
		fd[i] = gem_reopen_driver(device);
		assert_reset_status(i, fd[i], 0, RS_NO_ERROR);
	}

	sync_gpu();
	for (i = 0; i < num_fds; i++) {
		if (i == hang_index)
			inject_hang(fd[i], 0, e, ASYNC);
		else
			igt_assert(noop(fd[i], 0, e) > 0);
	}
	sync_gpu();

	for (i = 0; i < num_fds; i++) {
		if (hang_index < 0) {
			assert_reset_status(i, fd[i], 0, rs_assumed_no_hang);
			continue;
		}

		if (i < hang_index)
			assert_reset_status(i, fd[i], 0, RS_NO_ERROR);
		if (i == hang_index)
			assert_reset_status(i, fd[i], 0, RS_BATCH_ACTIVE);
		if (i > hang_index)
			assert_reset_status(i, fd[i], 0, RS_NO_ERROR);
	}

	igt_assert(igt_seconds_elapsed(&ts_injected) <= 30);

	for (i = 0; i < num_fds; i++)
		close(fd[i]);
}

#define MAX_CTX 100
static void test_rs_ctx(const struct intel_execution_ring *e,
			int num_fds, int num_ctx, int hang_index,
			int hang_context)
{
	int i, j;
	int fd[MAX_FD];
	int ctx[MAX_FD][MAX_CTX];

	igt_assert_lte(num_fds, MAX_FD);
	igt_assert_lt(hang_index, MAX_FD);

	igt_assert_lte(num_ctx, MAX_CTX);
	igt_assert_lt(hang_context, MAX_CTX);

	test_rs(e, num_fds, -1, RS_NO_ERROR);

	for (i = 0; i < num_fds; i++) {
		fd[i] = gem_reopen_driver(device);
		igt_assert(fd[i]);
		assert_reset_status(i, fd[i], 0, RS_NO_ERROR);

		for (j = 0; j < num_ctx; j++) {
			ctx[i][j] = gem_context_create(fd[i]);
		}

		assert_reset_status(i, fd[i], 0, RS_NO_ERROR);
	}

	for (i = 0; i < num_fds; i++) {
		assert_reset_status(i, fd[i], 0, RS_NO_ERROR);

		for (j = 0; j < num_ctx; j++)
			assert_reset_status(i, fd[i], ctx[i][j], RS_NO_ERROR);

		assert_reset_status(i, fd[i], 0, RS_NO_ERROR);
	}

	for (i = 0; i < num_fds; i++) {
		for (j = 0; j < num_ctx; j++) {
			if (i == hang_index && j == hang_context)
				inject_hang(fd[i], ctx[i][j], e, ASYNC);
			else
				igt_assert(noop(fd[i], ctx[i][j], e) > 0);
		}
	}
	sync_gpu();

	igt_assert(igt_seconds_elapsed(&ts_injected) <= 30);

	for (i = 0; i < num_fds; i++)
		assert_reset_status(i, fd[i], 0, RS_NO_ERROR);

	for (i = 0; i < num_fds; i++) {
		for (j = 0; j < num_ctx; j++) {
			if (i < hang_index)
				assert_reset_status(i, fd[i], ctx[i][j], RS_NO_ERROR);
			if (i == hang_index && j < hang_context)
				assert_reset_status(i, fd[i], ctx[i][j], RS_NO_ERROR);
			if (i == hang_index && j == hang_context)
				assert_reset_status(i, fd[i], ctx[i][j],
						    RS_BATCH_ACTIVE);
			if (i == hang_index && j > hang_context)
				assert_reset_status(i, fd[i], ctx[i][j],
						    RS_NO_ERROR);
			if (i > hang_index)
				assert_reset_status(i, fd[i], ctx[i][j],
						    RS_NO_ERROR);
		}
	}

	for (i = 0; i < num_fds; i++) {
		assert_reset_status(i, fd[i], 0, RS_NO_ERROR);
		close(fd[i]);
	}
}

static void test_ban(const struct intel_execution_ring *e)
{
	struct local_drm_i915_reset_stats rs_bad, rs_good;
	int fd_bad, fd_good;
	int ban, retry = 10;
	int active_count = 0;

	fd_bad = gem_reopen_driver(device);
	fd_good = gem_reopen_driver(device);

	assert_reset_status(fd_bad, fd_bad, 0, RS_NO_ERROR);
	assert_reset_status(fd_good, fd_good, 0, RS_NO_ERROR);

	noop(fd_bad, 0, e);
	noop(fd_good, 0, e);

	assert_reset_status(fd_bad, fd_bad, 0, RS_NO_ERROR);
	assert_reset_status(fd_good, fd_good, 0, RS_NO_ERROR);

	inject_hang(fd_bad, 0, e, BAN | ASYNC);
	active_count++;

	noop(fd_good, 0, e);
	noop(fd_good, 0, e);

	while (retry--) {
		inject_hang(fd_bad, 0, e, BAN);
		active_count++;

		ban = noop(fd_bad, 0, e);
		if (ban == -EIO)
			break;

		/* Should not happen often but sometimes hang is declared too
		 * slow due to our way of faking hang using loop */
		gem_close(fd_bad, ban);

		igt_info("retrying for ban (%d)\n", retry);
	}
	igt_assert_eq(ban, -EIO);
	igt_assert_lt(0, noop(fd_good, 0, e));

	assert_reset_status(fd_bad, fd_bad, 0, RS_BATCH_ACTIVE);
	igt_assert_eq(gem_reset_stats(fd_bad, 0, &rs_bad), 0);
	igt_assert_eq(rs_bad.batch_active, active_count);

	assert_reset_status(fd_good, fd_good, 0, RS_NO_ERROR);
	igt_assert_eq(gem_reset_stats(fd_good, 0, &rs_good), 0);
	igt_assert_eq(rs_good.batch_active, 0);

	close(fd_bad);
	close(fd_good);
}

static void test_ban_ctx(const struct intel_execution_ring *e)
{
	struct local_drm_i915_reset_stats rs_bad, rs_good;
	int fd, ban, retry = 10;
	uint32_t ctx_good, ctx_bad;
	int active_count = 0;

	fd = gem_reopen_driver(device);

	assert_reset_status(fd, fd, 0, RS_NO_ERROR);

	ctx_good = gem_context_create(fd);
	ctx_bad = gem_context_create(fd);

	assert_reset_status(fd, fd, 0, RS_NO_ERROR);
	assert_reset_status(fd, fd, ctx_good, RS_NO_ERROR);
	assert_reset_status(fd, fd, ctx_bad, RS_NO_ERROR);

	noop(fd, ctx_bad, e);
	noop(fd, ctx_good, e);

	assert_reset_status(fd, fd, ctx_good, RS_NO_ERROR);
	assert_reset_status(fd, fd, ctx_bad, RS_NO_ERROR);

	inject_hang(fd, ctx_bad, e, BAN | ASYNC);
	active_count++;

	noop(fd, ctx_good, e);
	noop(fd, ctx_good, e);

	while (retry--) {
		inject_hang(fd, ctx_bad, e, BAN);
		active_count++;

		ban = noop(fd, ctx_bad, e);
		if (ban == -EIO)
			break;

		/* Should not happen often but sometimes hang is declared too
		 * slow due to our way of faking hang using loop */
		gem_close(fd, ban);

		igt_info("retrying for ban (%d)\n", retry);
	}
	igt_assert_eq(ban, -EIO);
	igt_assert_lt(0, noop(fd, ctx_good, e));

	assert_reset_status(fd, fd, ctx_bad, RS_BATCH_ACTIVE);
	igt_assert_eq(gem_reset_stats(fd, ctx_bad, &rs_bad), 0);
	igt_assert_eq(rs_bad.batch_active, active_count);

	assert_reset_status(fd, fd, ctx_good, RS_NO_ERROR);
	igt_assert_eq(gem_reset_stats(fd, ctx_good, &rs_good), 0);
	igt_assert_eq(rs_good.batch_active, 0);

	close(fd);
}

static void test_unrelated_ctx(const struct intel_execution_ring *e)
{
	int fd1,fd2;
	int ctx_guilty, ctx_unrelated;

	fd1 = gem_reopen_driver(device);
	fd2 = gem_reopen_driver(device);
	assert_reset_status(0, fd1, 0, RS_NO_ERROR);
	assert_reset_status(1, fd2, 0, RS_NO_ERROR);
	ctx_guilty = gem_context_create(fd1);
	ctx_unrelated = gem_context_create(fd2);

	assert_reset_status(0, fd1, ctx_guilty, RS_NO_ERROR);
	assert_reset_status(1, fd2, ctx_unrelated, RS_NO_ERROR);

	inject_hang(fd1, ctx_guilty, e, 0);
	assert_reset_status(0, fd1, ctx_guilty, RS_BATCH_ACTIVE);
	assert_reset_status(1, fd2, ctx_unrelated, RS_NO_ERROR);

	gem_sync(fd2, noop(fd2, ctx_unrelated, e));
	assert_reset_status(0, fd1, ctx_guilty, RS_BATCH_ACTIVE);
	assert_reset_status(1, fd2, ctx_unrelated, RS_NO_ERROR);

	close(fd1);
	close(fd2);
}

static int get_reset_count(int fd, int ctx)
{
	int ret;
	struct local_drm_i915_reset_stats rs;

	ret = gem_reset_stats(fd, ctx, &rs);
	if (ret)
		return ret;

	return rs.reset_count;
}

static void test_close_pending_ctx(const struct intel_execution_ring *e)
{
	int fd = gem_reopen_driver(device);
	uint32_t ctx = gem_context_create(fd);

	assert_reset_status(fd, fd, ctx, RS_NO_ERROR);

	inject_hang(fd, ctx, e, 0);
	gem_context_destroy(fd, ctx);
	igt_assert_eq(__gem_context_destroy(fd, ctx), -ENOENT);

	close(fd);
}

static void test_close_pending(const struct intel_execution_ring *e)
{
	int fd = gem_reopen_driver(device);

	assert_reset_status(fd, fd, 0, RS_NO_ERROR);

	inject_hang(fd, 0, e, 0);
	close(fd);
}

static void noop_on_each_ring(int fd, const bool reverse)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_execbuffer2 eb;
	struct drm_i915_gem_exec_object2 obj;
	const struct intel_execution_ring *e;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	memset(&eb, 0, sizeof(eb));
	eb.buffers_ptr = to_user_pointer(&obj);
	eb.buffer_count = 1;

	if (reverse) {
		for (e = intel_execution_rings; e->name; e++)
			;
		while (--e >= intel_execution_rings) {
			eb.flags = eb_ring(e);
			__gem_execbuf(fd, &eb);
		}
	} else {
		for (e = intel_execution_rings; e->name; e++) {
			eb.flags = eb_ring(e);
			__gem_execbuf(fd, &eb);
		}
	}

	gem_sync(fd, obj.handle);
	gem_close(fd, obj.handle);
}

static void test_close_pending_fork(const struct intel_execution_ring *e,
				    const bool reverse)
{
	int fd = gem_reopen_driver(device);
	igt_hang_t hang;
	int pid;

	assert_reset_status(fd, fd, 0, RS_NO_ERROR);

	hang = igt_hang_ctx(fd, 0, eb_ring(e), 0);
	sleep(1);

	/* Avoid helpers as we need to kill the child
	 * without any extra signal handling on behalf of
	 * lib/drmtest.c
	 */
	pid = fork();
	if (pid == 0) {
		const int fd2 = gem_reopen_driver(device);
		igt_assert_lte(0, fd2);

		/* The crucial component is that we schedule the same noop batch
		 * on each ring. This exercises batch_obj reference counting,
		 * when gpu is reset and ring lists are cleared.
		 */
		noop_on_each_ring(fd2, reverse);
		close(fd2);
		pause();
		exit(0);
	} else {
		igt_assert_lt(0, pid);
		sleep(1);

		/* Kill the child to reduce refcounts on
		   batch_objs */
		kill(pid, SIGKILL);
	}

	igt_post_hang_ring(fd, hang);
	close(fd);
}

static void test_reset_count(const struct intel_execution_ring *e,
			     const bool create_ctx)
{
	int fd = gem_reopen_driver(device);
	int ctx;
	long c1, c2;

	if (create_ctx)
		ctx = gem_context_create(fd);
	else
		ctx = 0;

	assert_reset_status(fd, fd, ctx, RS_NO_ERROR);

	c1 = get_reset_count(fd, ctx);
	igt_assert(c1 >= 0);

	inject_hang(fd, ctx, e, 0);

	assert_reset_status(fd, fd, ctx, RS_BATCH_ACTIVE);
	c2 = get_reset_count(fd, ctx);
	igt_assert(c2 >= 0);
	igt_assert(c2 == (c1 + 1));

	igt_fork(child, 1) {
		igt_drop_root();

		c2 = get_reset_count(fd, ctx);

		igt_assert(c2 == 0);
	}

	igt_waitchildren();

	if (create_ctx)
		gem_context_destroy(fd, ctx);

	close(fd);
}

static int _test_params(int fd, int ctx, uint32_t flags, uint32_t pad)
{
	struct local_drm_i915_reset_stats rs;

	memset(&rs, 0, sizeof(rs));
	rs.ctx_id = ctx;
	rs.flags = flags;
	rs.reset_count = rand();
	rs.batch_active = rand();
	rs.batch_pending = rand();
	rs.pad = pad;

	if (drmIoctl(fd, GET_RESET_STATS_IOCTL, &rs))
		return -errno;

	return 0;
}

typedef enum { root = 0, user } cap_t;

static void _check_param_ctx(const int fd, const int ctx, const cap_t cap)
{
	const uint32_t bad = rand() + 1;

	if (ctx == 0) {
		igt_assert_eq(_test_params(fd, ctx, 0, 0), 0);

		if (cap != root) {
			igt_assert(get_reset_count(fd, ctx) == 0);
		}
	}

	igt_assert_eq(_test_params(fd, ctx, 0, bad), -EINVAL);
	igt_assert_eq(_test_params(fd, ctx, bad, 0), -EINVAL);
	igt_assert_eq(_test_params(fd, ctx, bad, bad), -EINVAL);
}

static void check_params(const int fd, const int ctx, cap_t cap)
{
	igt_assert(ioctl(fd, GET_RESET_STATS_IOCTL, 0) == -1);
	igt_assert_eq(_test_params(fd, 0xbadbad, 0, 0), -ENOENT);

	_check_param_ctx(fd, ctx, cap);
}

static void _test_param(const int fd, const int ctx)
{
	check_params(fd, ctx, root);

	igt_fork(child, 1) {
		check_params(fd, ctx, root);

		igt_drop_root();

		check_params(fd, ctx, user);
	}

	check_params(fd, ctx, root);

	igt_waitchildren();
}

static void test_params_ctx(void)
{
	int fd;

	fd = gem_reopen_driver(device);
	_test_param(fd, gem_context_create(fd));
	close(fd);
}

static void test_params(void)
{
	int fd;

	fd = gem_reopen_driver(device);
	_test_param(fd, 0);
	close(fd);
}

static const struct intel_execution_ring *
next_engine(int fd, const struct intel_execution_ring *e)
{
	do {
		e++;
		if (e->name == NULL)
			e = intel_execution_rings;
		if (e->exec_id == 0)
			e++;
	} while (!has_engine(fd, 0, e));

	return e;
}

static void defer_hangcheck(const struct intel_execution_ring *engine)
{
	const struct intel_execution_ring *next;
	int fd, count_start, count_end;
	int seconds = 30;

	fd = gem_reopen_driver(device);

	next = next_engine(fd, engine);
	igt_skip_on(next == engine);

	count_start = get_reset_count(fd, 0);
	igt_assert_lte(0, count_start);

	inject_hang(fd, 0, engine, 0);
	while (--seconds) {
		noop(fd, 0, next);

		count_end = get_reset_count(fd, 0);
		igt_assert_lte(0, count_end);

		if (count_end > count_start)
			break;

		sleep(1);
	}

	igt_assert_lt(count_start, count_end);

	close(fd);
}

static bool gem_has_reset_stats(int fd)
{
	struct local_drm_i915_reset_stats rs;
	int ret;

	/* Carefully set flags and pad to zero, otherwise
	   we get -EINVAL
	*/
	memset(&rs, 0, sizeof(rs));

	ret = drmIoctl(fd, GET_RESET_STATS_IOCTL, &rs);
	if (ret == 0)
		return true;

	/* If we get EPERM, we have support but did not
	   have CAP_SYSADM */
	if (ret == -1 && errno == EPERM)
		return true;

	return false;
}

static void test_shared_reset_domain(const intel_ctx_cfg_t *base_cfg,
		const struct intel_execution_engine2 *e)
{
	struct spin_ctx  __spin_ctx[GEM_MAX_ENGINES + 1];
	const struct intel_execution_engine2 *e2;
	struct gem_engine_properties params;
	int target_index = 0;
	int n_e = 0;

	sync_gpu();

	params.engine = e;
	params.preempt_timeout = 1;
	params.heartbeat_interval = 250;
	gem_engine_properties_configure(device, &params);

	for_each_ctx_cfg_engine(device, base_cfg, e2) {
		if (e2->flags == e->flags)
			target_index = n_e;

		__spin_ctx[n_e].class = e2->class;
		__spin_ctx[n_e].instance = e2->instance;

		/* Submits non preemptible workloads to all engines. */
		create_spinner(device, base_cfg, &__spin_ctx[n_e], e2->flags, -1023,
				IGT_SPIN_NO_PREEMPTION | IGT_SPIN_POLL_RUN | IGT_SPIN_FENCE_OUT);

		/* Checks the status of contexts submitted to engines. */
		assert_reset_status(device, device, __spin_ctx[n_e].ctx->id, RS_NO_ERROR);

		n_e++;
	}

	/* Submits preemptible workload to engine to be reset. */
	create_spinner(device, base_cfg, &__spin_ctx[n_e], e->flags, 1023, IGT_SPIN_POLL_RUN);

	/* Checks the status of preemptible context. */
	assert_reset_status(device, device, __spin_ctx[n_e].ctx->id, RS_NO_ERROR);

	igt_spin_free(device, __spin_ctx[n_e].spin);
	igt_assert_eq(sync_fence_wait(__spin_ctx[target_index].spin->out_fence, -1), 0);

	/* Checks the status of context after reset. */
	assert_reset_status(device, device, __spin_ctx[target_index].ctx->id, RS_BATCH_ACTIVE);

	for (int n = 0; n < n_e; n++) {
		/*
		 * If engine reset is RCS/CCS(dependent engines), then all the other
		 * contexts of RCS/CCS instances are victimised and rest contexts
		 * is of no error else if engine reset is not CCS/RCS then all the
		 * contexts should be of no error.
		 */
		struct spin_ctx *s = &__spin_ctx[n];

		igt_debug("Checking reset status for %d:%d\n", s->class, s->instance);
		if (n == target_index)
			continue;
		if ((e->class == I915_ENGINE_CLASS_COMPUTE ||
		     e->class == I915_ENGINE_CLASS_RENDER) &&
		    (s->class == I915_ENGINE_CLASS_COMPUTE ||
		     s->class == I915_ENGINE_CLASS_RENDER)) {
			igt_assert_eq(sync_fence_wait(s->spin->out_fence, -1), 0);
			assert_reset_status(device, device, s->ctx->id, RS_BATCH_ACTIVE);
		} else {
			assert_reset_status(device, device, s->ctx->id, RS_NO_ERROR);
		}
	}

	/* Cleanup. */
	for (int i = 0; i < n_e; i++) {
		igt_spin_free(device, __spin_ctx[i].spin);
		intel_ctx_destroy(device, __spin_ctx[i].ctx);
		put_ahnd(__spin_ctx[i].ahnd);
	}
	intel_ctx_destroy(device, __spin_ctx[n_e].ctx);
	put_ahnd(__spin_ctx[n_e].ahnd);

	sync_gpu();
	gem_engine_properties_restore(device, &params);
}

#define RUN_TEST(...) do { sync_gpu(); __VA_ARGS__; sync_gpu(); } while (0)
#define RUN_CTX_TEST(...) do { check_context(e); RUN_TEST(__VA_ARGS__); } while (0)

igt_main
{
	const struct intel_execution_ring *e;

	igt_fixture {
		bool has_reset_stats;
		bool using_full_reset;

		device = drm_open_driver(DRIVER_INTEL);
		devid = intel_get_drm_devid(device);

		enable_hangcheck(device, true);
		has_reset_stats = gem_has_reset_stats(device);

		igt_assert(igt_params_set(device, "reset", "%d", 1 /* only global reset */));

		using_full_reset = !gem_engine_reset_enabled(device) &&
				   gem_gpu_reset_enabled(device);

		igt_require_f(has_reset_stats,
			      "No reset stats ioctl support. Too old kernel?\n");
		igt_require_f(using_full_reset,
			      "Full GPU reset is not enabled. Is enable_hangcheck set?\n");
	}

	igt_subtest("params")
		test_params();

	igt_subtest_f("params-ctx")
		RUN_TEST(test_params_ctx());

	for (e = intel_execution_rings; e->name; e++) {
		igt_subtest_f("reset-stats-%s", e->name)
			RUN_TEST(test_rs(e, 4, 1, 0));

		igt_subtest_f("reset-stats-ctx-%s", e->name)
			RUN_CTX_TEST(test_rs_ctx(e, 4, 4, 1, 2));

		igt_subtest_f("ban-%s", e->name)
			RUN_TEST(test_ban(e));

		igt_subtest_f("ban-ctx-%s", e->name)
			RUN_CTX_TEST(test_ban_ctx(e));

		igt_subtest_f("reset-count-%s", e->name)
			RUN_TEST(test_reset_count(e, false));

		igt_subtest_f("reset-count-ctx-%s", e->name)
			RUN_CTX_TEST(test_reset_count(e, true));

		igt_subtest_f("unrelated-ctx-%s", e->name)
			RUN_CTX_TEST(test_unrelated_ctx(e));

		igt_subtest_f("close-pending-%s", e->name)
			RUN_TEST(test_close_pending(e));

		igt_subtest_f("close-pending-ctx-%s", e->name)
			RUN_CTX_TEST(test_close_pending_ctx(e));

		igt_subtest_f("close-pending-fork-%s", e->name)
			RUN_TEST(test_close_pending_fork(e, false));

		igt_subtest_f("close-pending-fork-reverse-%s", e->name)
			RUN_TEST(test_close_pending_fork(e, true));

		igt_subtest_f("defer-hangcheck-%s", e->name)
			RUN_TEST(defer_hangcheck(e));
	}

	igt_subtest_group {
		const struct intel_execution_engine2 *e2;
		intel_ctx_cfg_t cfg = {};

		igt_fixture {
			gem_require_contexts(device);
			cfg = intel_ctx_cfg_all_physical(device);

			igt_allow_hang(device, 0, 0);
			igt_assert(igt_params_set(device, "reset", "%u", -1));
			enable_hangcheck(device, false);
		}
		igt_subtest_with_dynamic("shared-reset-domain") {
			for_each_ctx_cfg_engine(device, &cfg, e2) {
				igt_dynamic_f("%s", e2->name)
					test_shared_reset_domain(&cfg, e2);
			}
		}
		igt_fixture {
			enable_hangcheck(device, true);
		}
	}
	igt_fixture {
		igt_assert(igt_params_set(device, "reset", "%d", INT_MAX /* any reset method */));
		close(device);
	}
}
