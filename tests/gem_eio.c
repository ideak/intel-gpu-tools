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
	igt_force_gpu_reset(fd);

	/* And just check the gpu is indeed running again */
	igt_debug("Checking that the GPU recovered\n");
	gem_quiescent_gpu(fd);
}

static void wedge_gpu(int fd)
{
	/* First idle the GPU then disable GPU resets before injecting a hang */
	gem_quiescent_gpu(fd);

	igt_require(i915_reset_control(false));

	igt_debug("Wedging GPU by injecting hang\n");
	igt_post_hang_ring(fd, igt_hang_ring(fd, I915_EXEC_DEFAULT));

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
	struct drm_i915_gem_wait wait;
	int err = 0;

	memset(&wait, 0, sizeof(wait));
	wait.bo_handle = handle;
	wait.timeout_ns = timeout;
	if (drmIoctl(fd, DRM_IOCTL_I915_GEM_WAIT, &wait))
		err = -errno;

	return err;
}

static void test_wait(int fd)
{
	igt_hang_t hang;

	/* If the request we wait on completes due to a hang (even for
	 * that request), the user expects the return value to 0 (success).
	 */
	hang = igt_hang_ring(fd, I915_EXEC_DEFAULT);
	igt_assert_eq(__gem_wait(fd, hang.handle, -1), 0);
	igt_post_hang_ring(fd, hang);

	/* If the GPU is wedged during the wait, again we expect the return
	 * value to be 0 (success).
	 */
	igt_require(i915_reset_control(false));
	hang = igt_hang_ring(fd, I915_EXEC_DEFAULT);
	igt_assert_eq(__gem_wait(fd, hang.handle, -1), 0);
	igt_post_hang_ring(fd, hang);
	igt_require(i915_reset_control(true));

	trigger_reset(fd);
}

static void test_inflight(int fd)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[2];
	uint32_t bbe = MI_BATCH_BUFFER_END;
	unsigned int engine;
	int fence[64]; /* conservative estimate of ring size */

	igt_require(gem_has_exec_fence(fd));

	memset(obj, 0, sizeof(obj));
	obj[0].flags = EXEC_OBJECT_WRITE;
	obj[1].handle = gem_create(fd, 4096);
	gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));

	for_each_engine(fd, engine) {
		igt_hang_t hang;

		igt_debug("Starting %s on engine '%s'\n", __func__, e__->name);
		igt_require(i915_reset_control(false));

		hang = igt_hang_ring(fd, engine);
		obj[0].handle = hang.handle;

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = to_user_pointer(obj);
		execbuf.buffer_count = 2;
		execbuf.flags = engine | I915_EXEC_FENCE_OUT;

		for (unsigned int n = 0; n < ARRAY_SIZE(fence); n++) {
			gem_execbuf_wr(fd, &execbuf);
			fence[n] = execbuf.rsvd2 >> 32;
			igt_assert(fence[n] != -1);
		}

		igt_post_hang_ring(fd, hang);

		igt_assert_eq(__gem_wait(fd, obj[1].handle, -1), 0);
		for (unsigned int n = 0; n < ARRAY_SIZE(fence); n++) {
			igt_assert_eq(sync_fence_status(fence[n]), -EIO);
			close(fence[n]);
		}

		igt_assert(i915_reset_control(true));
		trigger_reset(fd);
	}
}

static void test_inflight_suspend(int fd)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[2];
	uint32_t bbe = MI_BATCH_BUFFER_END;
	int fence[64]; /* conservative estimate of ring size */
	igt_hang_t hang;

	igt_require(gem_has_exec_fence(fd));
	igt_require(i915_reset_control(false));

	memset(obj, 0, sizeof(obj));
	obj[0].flags = EXEC_OBJECT_WRITE;
	obj[1].handle = gem_create(fd, 4096);
	gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));

	hang = igt_hang_ring(fd, 0);
	obj[0].handle = hang.handle;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	execbuf.flags = I915_EXEC_FENCE_OUT;

	for (unsigned int n = 0; n < ARRAY_SIZE(fence); n++) {
		gem_execbuf_wr(fd, &execbuf);
		fence[n] = execbuf.rsvd2 >> 32;
		igt_assert(fence[n] != -1);
	}

	igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
				      SUSPEND_TEST_DEVICES);

	igt_post_hang_ring(fd, hang);

	igt_assert_eq(__gem_wait(fd, obj[1].handle, -1), 0);
	for (unsigned int n = 0; n < ARRAY_SIZE(fence); n++) {
		igt_assert_eq(sync_fence_status(fence[n]), -EIO);
		close(fence[n]);
	}

	igt_assert(i915_reset_control(true));
	trigger_reset(fd);
}

static uint32_t __gem_context_create(int fd)
{
	struct drm_i915_gem_context_create create;

	memset(&create, 0, sizeof(create));
	if (ioctl(fd, DRM_IOCTL_I915_GEM_CONTEXT_CREATE, &create))
		return 0;

	return create.ctx_id;
}

static void test_inflight_contexts(int fd)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[2];
	uint32_t bbe = MI_BATCH_BUFFER_END;
	unsigned int engine;
	uint32_t ctx[64];
	int fence[64];

	igt_require(gem_has_exec_fence(fd));

	ctx[0] = __gem_context_create(fd);
	igt_require(ctx[0]);
	for (unsigned int n = 1; n < ARRAY_SIZE(ctx); n++)
		ctx[n] = gem_context_create(fd);

	memset(obj, 0, sizeof(obj));
	obj[0].flags = EXEC_OBJECT_WRITE;
	obj[1].handle = gem_create(fd, 4096);
	gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));

	for_each_engine(fd, engine) {
		igt_hang_t hang;

		igt_debug("Starting %s on engine '%s'\n", __func__, e__->name);
		igt_require(i915_reset_control(false));

		hang = igt_hang_ring(fd, engine);
		obj[0].handle = hang.handle;

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

		igt_post_hang_ring(fd, hang);

		igt_assert_eq(__gem_wait(fd, obj[1].handle, -1), 0);
		for (unsigned int n = 0; n < ARRAY_SIZE(fence); n++) {
			igt_assert_eq(sync_fence_status(fence[n]), -EIO);
			close(fence[n]);
		}

		igt_assert(i915_reset_control(true));
		trigger_reset(fd);
	}

	for (unsigned int n = 0; n < ARRAY_SIZE(ctx); n++)
		gem_context_destroy(fd, ctx[n]);
}

static void test_inflight_external(int fd)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj;
	uint32_t bbe = MI_BATCH_BUFFER_END;
	igt_hang_t hang;
	int timeline, fence;

	igt_require_sw_sync();
	igt_require(gem_has_exec_fence(fd));

	timeline = sw_sync_timeline_create();
	fence = sw_sync_timeline_create_fence(timeline, 1);

	igt_require(i915_reset_control(false));
	hang = igt_hang_ring(fd, I915_EXEC_DEFAULT);

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

	igt_post_hang_ring(fd, hang); /* wedged, with an unready batch */
	sw_sync_timeline_inc(timeline, 1); /* only now submit our batches */

	igt_assert_eq(__gem_wait(fd, obj.handle, -1), 0);
	igt_assert_eq(sync_fence_status(fence), -EIO);
	close(fence);

	igt_assert(i915_reset_control(true));
	trigger_reset(fd);
	close(timeline);
}

static void test_inflight_internal(int fd)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[2];
	uint32_t bbe = MI_BATCH_BUFFER_END;
	unsigned engine, nfence = 0;
	int fences[16];
	igt_hang_t hang;

	igt_require(gem_has_exec_fence(fd));

	igt_require(i915_reset_control(false));
	hang = igt_hang_ring(fd, I915_EXEC_DEFAULT);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = hang.handle;
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

	igt_post_hang_ring(fd, hang); /* wedged, with an unready batch */

	igt_assert_eq(__gem_wait(fd, obj[1].handle, -1), 0);
	while (nfence--) {
		igt_assert_eq(sync_fence_status(fences[nfence]), -EIO);
		close(fences[nfence]);
	}

	igt_assert(i915_reset_control(true));
	trigger_reset(fd);
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

		gem_show_submission_method(fd);
		igt_require_gem(fd);
		igt_require_hang_ring(fd, I915_EXEC_DEFAULT);
	}

	igt_subtest("throttle")
		test_throttle(fd);

	igt_subtest("execbuf")
		test_execbuf(fd);

	igt_subtest("wait")
		test_wait(fd);

	igt_subtest("in-flight")
		test_inflight(fd);

	igt_subtest("in-flight-contexts")
		test_inflight_contexts(fd);

	igt_subtest("in-flight-external")
		test_inflight_external(fd);

	igt_subtest("in-flight-internal") {
		igt_skip_on(gem_has_semaphores(fd));
		test_inflight_internal(fd);
	}

	igt_subtest("in-flight-suspend")
		test_inflight_suspend(fd);
}
