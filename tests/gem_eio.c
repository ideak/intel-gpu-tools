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
 */

/*
 * Testcase: Test that only specific ioctl report a wedged GPU.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <time.h>

#include <drm.h>

#include "igt.h"
#include "igt_sysfs.h"
#include "sw_sync.h"

IGT_TEST_DESCRIPTION("Test that specific ioctls report a wedged GPU (EIO).");

static bool i915_reset_control(bool enable)
{
	const char *path = "/sys/module/i915/parameters/reset";
	int fd, ret;

	igt_debug("%s GPU reset\n", enable ? "Enabling" : "Disabling");

	fd = open(path, O_RDWR);
	igt_require(fd >= 0);

	ret = write(fd, &"01"[enable], 1) == 1;
	close(fd);

	return ret;
}

static void trigger_reset(int fd)
{
	struct timespec ts = { };

	igt_nsec_elapsed(&ts);

	igt_force_gpu_reset(fd);

	/* And just check the gpu is indeed running again */
	igt_debug("Checking that the GPU recovered\n");
	gem_test_engine(fd, ALL_ENGINES);

	gem_quiescent_gpu(fd);

	/* We expect forced reset and health check to be quick. */
	igt_assert(igt_seconds_elapsed(&ts) < 2);
}

static void manual_hang(int drm_fd)
{
	int dir = igt_debugfs_dir(drm_fd);

	igt_sysfs_set(dir, "i915_wedged", "-1");

	close(dir);
}

static void wedge_gpu(int fd)
{
	/* First idle the GPU then disable GPU resets before injecting a hang */
	gem_quiescent_gpu(fd);

	igt_require(i915_reset_control(false));
	manual_hang(fd);
	igt_assert(i915_reset_control(true));
}

static int __gem_throttle(int fd)
{
	int err = 0;
	if (drmIoctl(fd, DRM_IOCTL_I915_GEM_THROTTLE, NULL))
		err = -errno;
	return err;
}

static void test_throttle(int fd)
{
	wedge_gpu(fd);

	igt_assert_eq(__gem_throttle(fd), -EIO);

	trigger_reset(fd);
}

static void test_execbuf(int fd)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 exec;
	uint32_t tmp[] = { MI_BATCH_BUFFER_END };

	memset(&exec, 0, sizeof(exec));
	memset(&execbuf, 0, sizeof(execbuf));

	exec.handle = gem_create(fd, 4096);
	gem_write(fd, exec.handle, 0, tmp, sizeof(tmp));

	execbuf.buffers_ptr = to_user_pointer(&exec);
	execbuf.buffer_count = 1;

	wedge_gpu(fd);

	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EIO);
	gem_close(fd, exec.handle);

	trigger_reset(fd);
}

static int __gem_wait(int fd, uint32_t handle, int64_t timeout)
{
	struct drm_i915_gem_wait wait = {
		.bo_handle = handle,
		.timeout_ns = timeout,
	};
	int err;

	err = 0;
	if (drmIoctl(fd, DRM_IOCTL_I915_GEM_WAIT, &wait))
		err = -errno;

	errno = 0;
	return err;
}

static igt_spin_t * __spin_poll(int fd, uint32_t ctx, unsigned long flags)
{
	if (gem_can_store_dword(fd, flags))
		return __igt_spin_batch_new_poll(fd, ctx, flags);
	else
		return __igt_spin_batch_new(fd, ctx, flags, 0);
}

static void __spin_wait(int fd, igt_spin_t *spin)
{
	if (spin->running) {
		igt_spin_busywait_until_running(spin);
	} else {
		igt_debug("__spin_wait - usleep mode\n");
		usleep(500e3); /* Better than nothing! */
	}
}

static igt_spin_t * spin_sync(int fd, uint32_t ctx, unsigned long flags)
{
	igt_spin_t *spin = __spin_poll(fd, ctx, flags);

	__spin_wait(fd, spin);

	return spin;
}

struct hang_ctx {
	int debugfs;
	struct timespec ts;
	timer_t timer;
};

static void hang_handler(union sigval arg)
{
	struct hang_ctx *ctx = arg.sival_ptr;

	igt_debug("hang delay = %.2fus\n", igt_nsec_elapsed(&ctx->ts) / 1000.0);

	igt_assert(igt_sysfs_set(ctx->debugfs, "i915_wedged", "-1"));

	igt_assert_eq(timer_delete(ctx->timer), 0);
	close(ctx->debugfs);
	free(ctx);
}

static void hang_after(int fd, unsigned int us)
{
	struct sigevent sev = {
		.sigev_notify = SIGEV_THREAD,
		.sigev_notify_function = hang_handler
	};
	struct itimerspec its = {
		.it_value.tv_sec = us / USEC_PER_SEC,
		.it_value.tv_nsec = us % USEC_PER_SEC * 1000,
	};
	struct hang_ctx *ctx;

	ctx = calloc(1, sizeof(*ctx));
	igt_assert(ctx);

	ctx->debugfs = igt_debugfs_dir(fd);
	igt_assert_fd(ctx->debugfs);

	sev.sigev_value.sival_ptr = ctx;

	igt_assert_eq(timer_create(CLOCK_MONOTONIC, &sev, &ctx->timer), 0);

	igt_nsec_elapsed(&ctx->ts);

	igt_assert_eq(timer_settime(ctx->timer, 0, &its, NULL), 0);
}

static int __check_wait(int fd, uint32_t bo, unsigned int wait)
{
	unsigned long wait_timeout = 250e6; /* Some margin for actual reset. */
	int ret;

	if (wait) {
		/*
		 * Double the wait plus some fixed margin to ensure gem_wait
		 * can never time out before the async hang runs.
		 */
		wait_timeout += wait * 2000 + 250e6;
		hang_after(fd, wait);
	} else {
		manual_hang(fd);
	}

	ret = __gem_wait(fd, bo, wait_timeout);

	return ret;
}

#define TEST_WEDGE (1)

static void test_wait(int fd, unsigned int flags, unsigned int wait)
{
	igt_spin_t *hang;

	fd = gem_reopen_driver(fd);
	igt_require_gem(fd);

	/*
	 * If the request we wait on completes due to a hang (even for
	 * that request), the user expects the return value to 0 (success).
	 */

	if (flags & TEST_WEDGE)
		igt_require(i915_reset_control(false));
	else
		igt_require(i915_reset_control(true));

	hang = spin_sync(fd, 0, I915_EXEC_DEFAULT);

	igt_assert_eq(__check_wait(fd, hang->handle, wait), 0);

	igt_spin_batch_free(fd, hang);

	igt_require(i915_reset_control(true));

	trigger_reset(fd);
	close(fd);
}

static void test_suspend(int fd, int state)
{
	fd = gem_reopen_driver(fd);
	igt_require_gem(fd);

	/* Do a suspend first so that we don't skip inside the test */
	igt_system_suspend_autoresume(state, SUSPEND_TEST_DEVICES);

	/* Check we can suspend when the driver is already wedged */
	igt_require(i915_reset_control(false));
	manual_hang(fd);

	igt_system_suspend_autoresume(state, SUSPEND_TEST_DEVICES);

	igt_require(i915_reset_control(true));
	trigger_reset(fd);
	close(fd);
}

static void test_inflight(int fd, unsigned int wait)
{
	int parent_fd = fd;
	unsigned int engine;

	igt_require_gem(fd);
	igt_require(gem_has_exec_fence(fd));

	for_each_engine(parent_fd, engine) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct drm_i915_gem_exec_object2 obj[2];
		struct drm_i915_gem_execbuffer2 execbuf;
		igt_spin_t *hang;
		int fence[64]; /* conservative estimate of ring size */

		fd = gem_reopen_driver(parent_fd);
		igt_require_gem(fd);

		memset(obj, 0, sizeof(obj));
		obj[0].flags = EXEC_OBJECT_WRITE;
		obj[1].handle = gem_create(fd, 4096);
		gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));

		gem_quiescent_gpu(fd);
		igt_debug("Starting %s on engine '%s'\n", __func__, e__->name);
		igt_require(i915_reset_control(false));

		hang = spin_sync(fd, 0, engine);
		obj[0].handle = hang->handle;

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = to_user_pointer(obj);
		execbuf.buffer_count = 2;
		execbuf.flags = engine | I915_EXEC_FENCE_OUT;

		for (unsigned int n = 0; n < ARRAY_SIZE(fence); n++) {
			gem_execbuf_wr(fd, &execbuf);
			fence[n] = execbuf.rsvd2 >> 32;
			igt_assert(fence[n] != -1);
		}

		igt_assert_eq(__check_wait(fd, obj[1].handle, wait), 0);

		for (unsigned int n = 0; n < ARRAY_SIZE(fence); n++) {
			igt_assert_eq(sync_fence_status(fence[n]), -EIO);
			close(fence[n]);
		}

		igt_spin_batch_free(fd, hang);
		igt_assert(i915_reset_control(true));
		trigger_reset(fd);

		gem_close(fd, obj[1].handle);
		close(fd);
	}
}

static void test_inflight_suspend(int fd)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[2];
	uint32_t bbe = MI_BATCH_BUFFER_END;
	int fence[64]; /* conservative estimate of ring size */
	igt_spin_t *hang;

	fd = gem_reopen_driver(fd);
	igt_require_gem(fd);
	igt_require(gem_has_exec_fence(fd));
	igt_require(i915_reset_control(false));

	memset(obj, 0, sizeof(obj));
	obj[0].flags = EXEC_OBJECT_WRITE;
	obj[1].handle = gem_create(fd, 4096);
	gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));

	hang = spin_sync(fd, 0, 0);
	obj[0].handle = hang->handle;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	execbuf.flags = I915_EXEC_FENCE_OUT;

	for (unsigned int n = 0; n < ARRAY_SIZE(fence); n++) {
		gem_execbuf_wr(fd, &execbuf);
		fence[n] = execbuf.rsvd2 >> 32;
		igt_assert(fence[n] != -1);
	}

	igt_set_autoresume_delay(30);
	igt_system_suspend_autoresume(SUSPEND_STATE_MEM, SUSPEND_TEST_NONE);

	igt_assert_eq(__check_wait(fd, obj[1].handle, 10), 0);

	for (unsigned int n = 0; n < ARRAY_SIZE(fence); n++) {
		igt_assert_eq(sync_fence_status(fence[n]), -EIO);
		close(fence[n]);
	}

	igt_spin_batch_free(fd, hang);
	igt_assert(i915_reset_control(true));
	trigger_reset(fd);
	close(fd);
}

static uint32_t context_create_safe(int i915)
{
	struct drm_i915_gem_context_param param;

	memset(&param, 0, sizeof(param));

	param.ctx_id = gem_context_create(i915);
	param.param = I915_CONTEXT_PARAM_BANNABLE;
	gem_context_set_param(i915, &param);

	param.param = I915_CONTEXT_PARAM_NO_ERROR_CAPTURE;
	param.value = 1;
	gem_context_set_param(i915, &param);

	return param.ctx_id;
}

static void test_inflight_contexts(int fd, unsigned int wait)
{
	int parent_fd = fd;
	unsigned int engine;

	igt_require_gem(fd);
	igt_require(gem_has_exec_fence(fd));
	gem_require_contexts(fd);

	for_each_engine(parent_fd, engine) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct drm_i915_gem_exec_object2 obj[2];
		struct drm_i915_gem_execbuffer2 execbuf;
		igt_spin_t *hang;
		uint32_t ctx[64];
		int fence[64];

		fd = gem_reopen_driver(parent_fd);
		igt_require_gem(fd);

		for (unsigned int n = 0; n < ARRAY_SIZE(ctx); n++)
			ctx[n] = context_create_safe(fd);

		gem_quiescent_gpu(fd);

		igt_debug("Starting %s on engine '%s'\n", __func__, e__->name);
		igt_require(i915_reset_control(false));

		memset(obj, 0, sizeof(obj));
		obj[0].flags = EXEC_OBJECT_WRITE;
		obj[1].handle = gem_create(fd, 4096);
		gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));

		hang = spin_sync(fd, 0, engine);
		obj[0].handle = hang->handle;

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = to_user_pointer(obj);
		execbuf.buffer_count = 2;
		execbuf.flags = engine | I915_EXEC_FENCE_OUT;

		for (unsigned int n = 0; n < ARRAY_SIZE(fence); n++) {
			execbuf.rsvd1 = ctx[n];
			gem_execbuf_wr(fd, &execbuf);
			fence[n] = execbuf.rsvd2 >> 32;
			igt_assert(fence[n] != -1);
		}

		igt_assert_eq(__check_wait(fd, obj[1].handle, wait), 0);

		for (unsigned int n = 0; n < ARRAY_SIZE(fence); n++) {
			igt_assert_eq(sync_fence_status(fence[n]), -EIO);
			close(fence[n]);
		}

		igt_spin_batch_free(fd, hang);
		gem_close(fd, obj[1].handle);
		igt_assert(i915_reset_control(true));
		trigger_reset(fd);

		for (unsigned int n = 0; n < ARRAY_SIZE(ctx); n++)
			gem_context_destroy(fd, ctx[n]);

		close(fd);
	}
}

static void test_inflight_external(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj;
	igt_spin_t *hang;
	uint32_t fence;
	IGT_CORK_FENCE(cork);

	igt_require_sw_sync();
	igt_require(gem_has_exec_fence(fd));

	fd = gem_reopen_driver(fd);
	igt_require_gem(fd);

	fence = igt_cork_plug(&cork, fd);

	igt_require(i915_reset_control(false));
	hang = __spin_poll(fd, 0, 0);

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags = I915_EXEC_FENCE_IN | I915_EXEC_FENCE_OUT;
	execbuf.rsvd2 = (uint32_t)fence;

	gem_execbuf_wr(fd, &execbuf);
	close(fence);

	fence = execbuf.rsvd2 >> 32;
	igt_assert(fence != -1);

	__spin_wait(fd, hang);
	manual_hang(fd);

	gem_sync(fd, hang->handle); /* wedged, with an unready batch */
	igt_assert(!gem_bo_busy(fd, hang->handle));
	igt_assert(gem_bo_busy(fd, obj.handle));
	igt_cork_unplug(&cork); /* only now submit our batches */

	igt_assert_eq(__gem_wait(fd, obj.handle, -1), 0);
	igt_assert_eq(sync_fence_status(fence), -EIO);
	close(fence);

	igt_spin_batch_free(fd, hang);
	igt_assert(i915_reset_control(true));
	trigger_reset(fd);
	close(fd);
}

static void test_inflight_internal(int fd, unsigned int wait)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[2];
	uint32_t bbe = MI_BATCH_BUFFER_END;
	unsigned engine, nfence = 0;
	int fences[16];
	igt_spin_t *hang;

	igt_require(gem_has_exec_fence(fd));

	fd = gem_reopen_driver(fd);
	igt_require_gem(fd);

	igt_require(i915_reset_control(false));
	hang = spin_sync(fd, 0, 0);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = hang->handle;
	obj[0].flags = EXEC_OBJECT_WRITE;
	obj[1].handle = gem_create(fd, 4096);
	gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	for_each_engine(fd, engine) {
		execbuf.flags = engine | I915_EXEC_FENCE_OUT;

		gem_execbuf_wr(fd, &execbuf);

		fences[nfence] = execbuf.rsvd2 >> 32;
		igt_assert(fences[nfence] != -1);
		nfence++;
	}

	igt_assert_eq(__check_wait(fd, obj[1].handle, wait), 0);

	while (nfence--) {
		igt_assert_eq(sync_fence_status(fences[nfence]), -EIO);
		close(fences[nfence]);
	}

	igt_spin_batch_free(fd, hang);
	igt_assert(i915_reset_control(true));
	trigger_reset(fd);
	close(fd);
}

/*
 * Verify that we can submit and execute work after unwedging the GPU.
 */
static void test_reset_stress(int fd, unsigned int flags)
{
	uint32_t ctx0 = gem_context_create(fd);

	igt_until_timeout(5) {
		struct drm_i915_gem_execbuffer2 execbuf = { };
		struct drm_i915_gem_exec_object2 obj = { };
		uint32_t bbe = MI_BATCH_BUFFER_END;
		igt_spin_t *hang;
		unsigned int i;
		uint32_t ctx;

		gem_quiescent_gpu(fd);

		igt_require(i915_reset_control(flags & TEST_WEDGE ?
					       false : true));

		ctx = context_create_safe(fd);

		/*
		 * Start executing a spin batch with some queued batches
		 * against a different context after it.
		 */
		hang = spin_sync(fd, ctx0, 0);

		obj.handle = gem_create(fd, 4096);
		gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

		execbuf.buffers_ptr = to_user_pointer(&obj);
		execbuf.buffer_count = 1;
		execbuf.rsvd1 = ctx0;

		for (i = 0; i < 10; i++)
			gem_execbuf(fd, &execbuf);

		/* Wedge after a small delay. */
		igt_assert_eq(__check_wait(fd, obj.handle, 100e3), 0);

		/* Unwedge by forcing a reset. */
		igt_assert(i915_reset_control(true));
		trigger_reset(fd);

		gem_quiescent_gpu(fd);

		/*
		 * Verify that we are able to submit work after unwedging from
		 * both contexts.
		 */
		execbuf.rsvd1 = ctx;
		for (i = 0; i < 5; i++)
			gem_execbuf(fd, &execbuf);

		execbuf.rsvd1 = ctx0;
		for (i = 0; i < 5; i++)
			gem_execbuf(fd, &execbuf);

		gem_sync(fd, obj.handle);
		igt_spin_batch_free(fd, hang);
		gem_context_destroy(fd, ctx);
		gem_close(fd, obj.handle);
	}

	gem_context_destroy(fd, ctx0);
}

static int fd = -1;

static void
exit_handler(int sig)
{
	i915_reset_control(true);
	igt_force_gpu_reset(fd);
}

igt_main
{
	igt_skip_on_simulation();

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);

		igt_require(i915_reset_control(true));
		igt_force_gpu_reset(fd);
		igt_install_exit_handler(exit_handler);

		gem_submission_print_method(fd);
		igt_require_gem(fd);

		igt_allow_hang(fd, 0, 0);
	}

	igt_subtest("throttle")
		test_throttle(fd);

	igt_subtest("execbuf")
		test_execbuf(fd);

	igt_subtest("suspend")
		test_suspend(fd, SUSPEND_STATE_MEM);

	igt_subtest("hibernate")
		test_suspend(fd, SUSPEND_STATE_DISK);

	igt_subtest("in-flight-external")
		test_inflight_external(fd);

	igt_subtest("in-flight-suspend")
		test_inflight_suspend(fd);

	igt_subtest("reset-stress")
		test_reset_stress(fd, 0);

	igt_subtest("unwedge-stress")
		test_reset_stress(fd, TEST_WEDGE);

	igt_subtest_group {
		const struct {
			unsigned int wait;
			const char *name;
		} waits[] = {
			{ .wait = 0, .name = "immediate" },
			{ .wait = 1, .name = "1us" },
			{ .wait = 10000, .name = "10ms" },
		};
		unsigned int i;

		for (i = 0; i < sizeof(waits) / sizeof(waits[0]); i++) {
			igt_subtest_f("wait-%s", waits[i].name)
				test_wait(fd, 0, waits[i].wait);

			igt_subtest_f("wait-wedge-%s", waits[i].name)
				test_wait(fd, TEST_WEDGE, waits[i].wait);

			igt_subtest_f("in-flight-%s", waits[i].name)
				test_inflight(fd, waits[i].wait);

			igt_subtest_f("in-flight-contexts-%s", waits[i].name)
				test_inflight_contexts(fd, waits[i].wait);

			igt_subtest_f("in-flight-internal-%s", waits[i].name) {
				igt_skip_on(gem_has_semaphores(fd));
				test_inflight_internal(fd, waits[i].wait);
			}
		}
	}
}
