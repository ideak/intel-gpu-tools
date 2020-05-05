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
#include <inttypes.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include "drmtest.h"
#include "i915/gem.h"
#include "i915/gem_context.h"
#include "i915/gem_engine_topology.h"
#include "ioctl_wrappers.h" /* gem_wait()! */
#include "sw_sync.h"

static bool has_ringsize(int i915)
{
	struct drm_i915_gem_context_param p = {
		.param = I915_CONTEXT_PARAM_RINGSIZE,
	};

	return __gem_context_get_param(i915, &p) == 0;
}

static void test_idempotent(int i915)
{
	struct drm_i915_gem_context_param p = {
		.param = I915_CONTEXT_PARAM_RINGSIZE,
	};
	uint32_t saved;

	/*
	 * Simple test to verify that we are able to read back the same
	 * value as we set.
	 */

	gem_context_get_param(i915, &p);
	saved = p.value;

	for (uint32_t x = 1 << 12; x <= 128 << 12; x <<= 1) {
		p.value = x;
		gem_context_set_param(i915, &p);
		gem_context_get_param(i915, &p);
		igt_assert_eq_u32(p.value, x);
	}

	p.value = saved;
	gem_context_set_param(i915, &p);
}

static void test_invalid(int i915)
{
	struct drm_i915_gem_context_param p = {
		.param = I915_CONTEXT_PARAM_RINGSIZE,
	};
	uint64_t invalid[] = {
		0, 1, 4095, 4097, 8191, 8193,
		/* upper limit may be HW dependent, atm it is 512KiB */
		(512 << 10) - 1, (512 << 10) + 1,
		-1, -1u
	};
	uint32_t saved;

	/*
	 * The HW only accepts certain aligned values and so we reject
	 * any invalid sizes specified by the user.
	 *
	 * Currently, the HW only accepts 4KiB - 512KiB in 4K increments,
	 * and is unlikely to ever accept smaller.
	 */

	gem_context_get_param(i915, &p);
	saved = p.value;

	for (int i = 0; i < ARRAY_SIZE(invalid); i++) {
		p.value = invalid[i];
		igt_assert_eq(__gem_context_set_param(i915, &p), -EINVAL);
		gem_context_get_param(i915, &p);
		igt_assert_eq_u64(p.value, saved);
	}
}

static int create_ext_ioctl(int i915,
			    struct drm_i915_gem_context_create_ext *arg)
{
	int err;

	err = 0;
	if (igt_ioctl(i915, DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT, arg)) {
		err = -errno;
		igt_assume(err);
	}

	errno = 0;
	return err;
}

static void test_create(int i915)
{
	struct drm_i915_gem_context_create_ext_setparam p = {
		.base = {
			.name = I915_CONTEXT_CREATE_EXT_SETPARAM,
			.next_extension = 0, /* end of chain */
		},
		.param = {
			.param = I915_CONTEXT_PARAM_RINGSIZE,
			.value = 512 << 10,
		}
	};
	struct drm_i915_gem_context_create_ext create = {
		.flags = I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS,
		.extensions = to_user_pointer(&p),
	};

	/*
	 * Check that the ringsize parameter is used during context constuction.
	 */

	igt_assert_eq(create_ext_ioctl(i915, &create),  0);

	p.param.ctx_id = create.ctx_id;
	p.param.value = 0;
	gem_context_get_param(i915, &p.param);
	igt_assert_eq(p.param.value, 512 << 10);

	gem_context_destroy(i915, create.ctx_id);
}

static void test_clone(int i915)
{
	struct drm_i915_gem_context_create_ext_setparam p = {
		.base = {
			.name = I915_CONTEXT_CREATE_EXT_SETPARAM,
			.next_extension = 0, /* end of chain */
		},
		.param = {
			.param = I915_CONTEXT_PARAM_RINGSIZE,
			.value = 512 << 10,
		}
	};
	struct drm_i915_gem_context_create_ext create = {
		.flags = I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS,
		.extensions = to_user_pointer(&p),
	};

	/*
	 * Check that the ringsize is copied across during context cloning.
	 */

	igt_assert_eq(create_ext_ioctl(i915, &create),  0);

	p.param.ctx_id = gem_context_clone(i915, create.ctx_id,
					   I915_CONTEXT_CLONE_ENGINES, 0);
	igt_assert_neq(p.param.ctx_id, create.ctx_id);
	gem_context_destroy(i915, create.ctx_id);

	p.param.value = 0;
	gem_context_get_param(i915, &p.param);
	igt_assert_eq(p.param.value, 512 << 10);

	gem_context_destroy(i915, p.param.ctx_id);
}

static int __execbuf(int i915, struct drm_i915_gem_execbuffer2 *execbuf)
{
	int err;

	err = 0;
	if (ioctl(i915, DRM_IOCTL_I915_GEM_EXECBUFFER2, execbuf)) {
		err = -errno;
		igt_assume(err);
	}

	errno = 0;
	return err;
}

static uint32_t __batch_create(int i915, uint32_t offset)
{
	const uint32_t bbe = 0xa << 23;
	uint32_t handle;

	handle = gem_create(i915, offset + sizeof(bbe));
	gem_write(i915, handle, offset, &bbe, sizeof(bbe));

	return handle;
}

static uint32_t batch_create(int i915)
{
	return __batch_create(i915, 0);
}

static unsigned int measure_inflight(int i915, unsigned int engine, int timeout)
{
	IGT_CORK_FENCE(cork);
	struct drm_i915_gem_exec_object2 obj = {
		.handle = batch_create(i915)
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = engine | I915_EXEC_FENCE_IN,
		.rsvd2 = igt_cork_plug(&cork, i915),
	};
	unsigned int count;
	int err;

	fcntl(i915, F_SETFL, fcntl(i915, F_GETFL) | O_NONBLOCK);
	igt_set_timeout(timeout, "execbuf blocked!");

	gem_execbuf(i915, &execbuf);
	for (count = 1; (err = __execbuf(i915, &execbuf)) == 0; count++)
		;
	igt_assert_eq(err, -EWOULDBLOCK);
	close(execbuf.rsvd2);

	igt_reset_timeout();
	fcntl(i915, F_SETFL, fcntl(i915, F_GETFL) & ~O_NONBLOCK);

	igt_cork_unplug(&cork);
	gem_close(i915, obj.handle);

	return count;
}

static void test_resize(int i915,
			const struct intel_execution_engine2 *e,
			void *data)
#define IDLE (1 << 0)
#define as_pointer(x) (void *)(uintptr_t)(x)
{
	struct drm_i915_gem_context_param p = {
		.param = I915_CONTEXT_PARAM_RINGSIZE,
	};
	unsigned long flags = (uintptr_t)data;
	unsigned int prev[2] = {};
	uint64_t elapsed;
	uint32_t saved;

	/*
	 * The ringsize directly affects the number of batches we can have
	 * inflight -- when we run out of room in the ring, the client is
	 * blocked (or if O_NONBLOCK is specified, -EWOULDBLOCK is reported).
	 * The kernel throttles the client when they enter the last 4KiB page,
	 * so as we double the size of the ring, we nearly double the number
	 * of requests we can fit as 2^n-1: i.e 0, 1, 3, 7, 15, 31 pages.
	 */

	gem_context_get_param(i915, &p);
	saved = p.value;

	/* XXX disable hangchecking? */
	elapsed = 0;
	gem_quiescent_gpu(i915);
	for (p.value = 1 << 12; p.value <= 128 << 12; p.value <<= 1) {
		struct timespec tv = {};
		unsigned int count;

		gem_context_set_param(i915, &p);

		igt_nsec_elapsed(&tv);
		count = measure_inflight(i915, e->flags, 1 + ceil(2 * elapsed*1e-9));
		elapsed = igt_nsec_elapsed(&tv);

		igt_info("%s: %6llx -> %'6d\n", e->name, p.value, count);
		igt_assert(count > 3 * (prev[1] - prev[0]) / 4 + prev[1]);
		if (flags & IDLE)
			gem_quiescent_gpu(i915);

		prev[0] = prev[1];
		prev[1] = count;
	}
	gem_quiescent_gpu(i915);

	p.value = saved;
	gem_context_set_param(i915, &p);
}

static void gem_test_each_engine(int i915, const char *name,
				 void (*fn)(int i915,
					    const struct intel_execution_engine2 *e,
					    void *data),
				 void *data)
{
	const struct intel_execution_engine2 *e;

	igt_subtest_with_dynamic(name) {
		__for_each_physical_engine(i915, e) {
			igt_dynamic_f("%s", e->name)
				fn(i915, e, data);
		}
	}
}

igt_main
{
	int i915;

	igt_fixture {
		i915 = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(i915);

		igt_require(has_ringsize(i915));
	}

	igt_subtest("idempotent")
		test_idempotent(i915);

	igt_subtest("invalid")
		test_invalid(i915);

	igt_subtest("create")
		test_create(i915);
	igt_subtest("clone")
		test_clone(i915);

	gem_test_each_engine(i915, "idle", test_resize, as_pointer(IDLE));
	gem_test_each_engine(i915, "active", test_resize, 0);

	/* XXX ctx->engines[]? Clone (above) should be enough */

	igt_fixture {
		close(i915);
	}
}
