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
 *    Daniel Vetter <daniel.vetter@ffwll.ch>
 */

#include <fcntl.h>
#include <limits.h>

#include "igt.h"
#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/gem_vm.h"

IGT_TEST_DESCRIPTION("Basic test for context set/get param input validation.");

#define NEW_CTX	BIT(0)
#define USER BIT(1)

static void set_priority(int i915)
{
	static const int64_t test_values[] = {
		/* Test space too big, pick significant values */
		INT_MIN,

		I915_CONTEXT_MIN_USER_PRIORITY - 1,
		I915_CONTEXT_MIN_USER_PRIORITY,
		I915_CONTEXT_MIN_USER_PRIORITY + 1,

		I915_CONTEXT_DEFAULT_PRIORITY - 1,
		I915_CONTEXT_DEFAULT_PRIORITY,
		I915_CONTEXT_DEFAULT_PRIORITY + 1,

		I915_CONTEXT_MAX_USER_PRIORITY - 1,
		I915_CONTEXT_MAX_USER_PRIORITY,
		I915_CONTEXT_MAX_USER_PRIORITY + 1,

		INT_MAX
	};
	unsigned int size;
	int64_t *values;

	igt_require(getuid() == 0);

	size = ARRAY_SIZE(test_values);
	values = malloc(sizeof(test_values) * 8);
	igt_assert(values);

	for (unsigned i = 0; i < size; i++) {
		values[i + 0*size] = test_values[i];
		values[i + 1*size] = test_values[i] | (uint64_t)1 << 32;
		values[i + 2*size] = test_values[i] | (uint64_t)rand() << 32;
		values[i + 3*size] = test_values[i] ^ rand();
		values[i + 4*size] = rand() % (I915_CONTEXT_MAX_USER_PRIORITY - I915_CONTEXT_MIN_USER_PRIORITY) + I915_CONTEXT_MIN_USER_PRIORITY;
		values[i + 5*size] = rand();
		values[i + 6*size] = rand() | (uint64_t)rand() << 32;
		values[i + 7*size] = (uint64_t)test_values[i] << 32;
	}
	size *= 8;

	igt_permute_array(values, size, igt_exchange_int64);

	igt_fork(flags, NEW_CTX | USER) {
		int fd = gem_reopen_driver(i915);
		struct drm_i915_gem_context_param arg = {
			.param = I915_CONTEXT_PARAM_PRIORITY,
			.ctx_id = flags & NEW_CTX ? gem_context_create(fd) : 0,
		};
		int64_t old_prio;

		if (flags & USER) {
			igt_debug("Dropping root privilege\n");
			igt_drop_root();
		}

		gem_context_get_param(fd, &arg);
		old_prio = arg.value;

		for (unsigned i = 0; i < size; i++) {
			int64_t prio = values[i];
			int expected = 0;
			int err;

			arg.value = prio;

			if (flags & USER &&
			    prio > I915_CONTEXT_DEFAULT_PRIORITY)
				expected = -EPERM;

			if (prio < I915_CONTEXT_MIN_USER_PRIORITY ||
			    prio > I915_CONTEXT_MAX_USER_PRIORITY)
				expected = -EINVAL;

			err =__gem_context_set_param(fd, &arg);
			igt_assert_f(err == expected,
				     "Priority requested %" PRId64 " with flags %x, expected result %d, returned %d\n",
				     prio, flags, expected, err);

			gem_context_get_param(fd, &arg);
			if (!err)
				old_prio = prio;
			igt_assert_eq(arg.value, old_prio);
		}

		arg.value = 0;
		gem_context_set_param(fd, &arg);

		if (flags & NEW_CTX)
			gem_context_destroy(fd, arg.ctx_id);
	}

	igt_waitchildren();
	free(values);
}

static uint32_t __batch_create(int i915, uint32_t offset)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle;

	handle = gem_create(i915, ALIGN(offset + 4, 4096));
	gem_write(i915, handle, offset, &bbe, sizeof(bbe));

	return handle;
}

static uint32_t batch_create(int i915)
{
	return __batch_create(i915, 0);
}

static void test_vm(int i915)
{
	const uint64_t nonzero_offset = 48 << 20;
	struct drm_i915_gem_exec_object2 batch = {
		.handle = batch_create(i915),
	};
	struct drm_i915_gem_execbuffer2 eb = {
		.buffers_ptr = to_user_pointer(&batch),
		.buffer_count = 1,
	};
	struct drm_i915_gem_context_param arg = {
		.param = I915_CONTEXT_PARAM_VM,
	};
	int err;
	uint32_t parent, child;
	igt_spin_t *spin;

	/*
	 * Proving 2 contexts share the same GTT is quite tricky as we have no
	 * means of directly comparing them (each handle returned to userspace
	 * is unique). What we do instead is rely on a quirk of execbuf that
	 * it does not try to move an VMA without good reason, and so that
	 * having used an object in one context, it will have the same address
	 * in the next context that shared the VM.
	 */

	arg.ctx_id = gem_context_create(i915);
	arg.value = -1ull;
	err = __gem_context_set_param(i915, &arg);
	gem_context_destroy(i915, arg.ctx_id);
	igt_require(err == -ENOENT);

	/* Test that we can't set the VM on ctx0 */
	arg.ctx_id = 0;
	arg.value = gem_vm_create(i915);
	err = __gem_context_set_param(i915, &arg);
	gem_vm_destroy(i915, arg.value);
	igt_assert_eq(err, -EINVAL);

	/* Test that we can't set the VM after we've done an execbuf */
	arg.ctx_id = gem_context_create(i915);
	spin = igt_spin_new(i915, .ctx_id = arg.ctx_id);
	igt_spin_free(i915, spin);
	arg.value = gem_vm_create(i915);
	err = __gem_context_set_param(i915, &arg);
	gem_context_destroy(i915, arg.ctx_id);
	gem_vm_destroy(i915, arg.value);
	igt_assert_eq(err, -EINVAL);

	parent = gem_context_create(i915);
	child = gem_context_create(i915);

	/* Create a background spinner to keep the engines busy */
	spin = igt_spin_new(i915);
	for (int i = 0; i < 16; i++) {
		spin->execbuf.rsvd1 = gem_context_create(i915);
		__gem_context_set_priority(i915, spin->execbuf.rsvd1, 1023);
		gem_execbuf(i915, &spin->execbuf);
		gem_context_destroy(i915, spin->execbuf.rsvd1);
	}

	/* Using implicit soft-pinning */
	eb.rsvd1 = parent;
	batch.offset = nonzero_offset;
	gem_execbuf(i915, &eb);
	igt_assert_eq_u64(batch.offset, nonzero_offset);

	eb.rsvd1 = child;
	batch.offset = 0;
	gem_execbuf(i915, &eb);
	igt_assert_eq_u64(batch.offset, 0);
	gem_context_destroy(i915, child);

	eb.rsvd1 = parent;
	gem_execbuf(i915, &eb);
	igt_assert_eq_u64(batch.offset, nonzero_offset);

	arg.ctx_id = parent;
	gem_context_get_param(i915, &arg);

	/* Note: changing an active ctx->vm may be verboten */
	child = gem_context_create(i915);
	arg.ctx_id = child;
	if (__gem_context_set_param(i915, &arg) != -EBUSY) {
		eb.rsvd1 = child;
		batch.offset = 0;
		gem_execbuf(i915, &eb);
		igt_assert_eq_u64(batch.offset, nonzero_offset);
	}

	gem_context_destroy(i915, child);
	gem_context_destroy(i915, parent);

	/* both contexts destroyed, but we still keep hold of the vm */
	child = gem_context_create(i915);

	arg.ctx_id = child;
	gem_context_set_param(i915, &arg);

	eb.rsvd1 = child;
	batch.offset = 0;
	gem_execbuf(i915, &eb);
	igt_assert_eq_u64(batch.offset, nonzero_offset);

	gem_context_destroy(i915, child);
	gem_vm_destroy(i915, arg.value);

	igt_spin_free(i915, spin);
	gem_sync(i915, batch.handle);
	gem_close(i915, batch.handle);
}

static void test_set_invalid_param(int fd, uint64_t param, uint64_t value)
{
	/* Create a fresh context */
	struct drm_i915_gem_context_param arg = {
		.ctx_id = gem_context_create(fd),
		.param = param,
		.value = value,
	};
	int err;

	err = __gem_context_set_param(fd, &arg);
	gem_context_destroy(fd, arg.ctx_id);
	igt_assert_eq(err, -EINVAL);
}

static void test_get_invalid_param(int fd, uint64_t param)
{
	/* Create a fresh context */
	struct drm_i915_gem_context_param arg = {
		.ctx_id = gem_context_create(fd),
		.param = param,
	};
	int err;

	err = __gem_context_get_param(fd, &arg);
	gem_context_destroy(fd, arg.ctx_id);
	igt_assert_eq(err, -EINVAL);
}

igt_main
{
	struct drm_i915_gem_context_param arg;
	int fd;
	uint32_t ctx;

	memset(&arg, 0, sizeof(arg));

	igt_fixture {
		fd = drm_open_driver_render(DRIVER_INTEL);

		gem_require_contexts(fd);
		ctx = gem_context_create(fd);

		arg.param = I915_CONTEXT_PARAM_BAN_PERIOD;

		/* XXX start to enforce ban period returning -EINVAL when
		 * transition has been done */
		if (__gem_context_get_param(fd, &arg) == -EINVAL)
			arg.param = I915_CONTEXT_PARAM_BANNABLE;
	}

	igt_describe("Basic test for context get/set param ioctls using valid context");
	igt_subtest("basic") {
		arg.ctx_id = ctx;
		gem_context_get_param(fd, &arg);
		gem_context_set_param(fd, &arg);
	}

	igt_describe("Basic test for context get/set param ioctls using default context");
	igt_subtest("basic-default") {
		arg.ctx_id = 0;
		gem_context_get_param(fd, &arg);
		gem_context_set_param(fd, &arg);
	}

	igt_describe("Verify that context get param ioctl using invalid context "
	       "returns relevant error");
	igt_subtest("invalid-ctx-get") {
		arg.ctx_id = 2;
		igt_assert_eq(__gem_context_get_param(fd, &arg), -ENOENT);
	}

	igt_describe("Verify that context set param ioctl using invalid context "
	       "returns relevant error");
	igt_subtest("invalid-ctx-set") {
		arg.ctx_id = ctx;
		gem_context_get_param(fd, &arg);
		arg.ctx_id = 2;
		igt_assert_eq(__gem_context_set_param(fd, &arg), -ENOENT);
	}

	igt_describe("Verify that context get param ioctl returns valid size for valid context");
	igt_subtest("invalid-size-get") {
		arg.ctx_id = ctx;
		arg.size = 8;
		gem_context_get_param(fd, &arg);
		igt_assert(arg.size == 0);
	}

	igt_describe("Verify that context set param ioctl using invalid size "
	       "returns relevant error");
	igt_subtest("invalid-size-set") {
		arg.ctx_id = ctx;
		gem_context_get_param(fd, &arg);
		arg.size = 8;
		igt_assert_eq(__gem_context_set_param(fd, &arg), -EINVAL);
		arg.size = 0;
	}

	igt_describe("Verify that context set param ioctl returns relevant error in non root mode");
	igt_subtest("non-root-set") {
		igt_fork(child, 1) {
			igt_drop_root();

			arg.ctx_id = ctx;
			gem_context_get_param(fd, &arg);
			arg.value--;
			igt_assert_eq(__gem_context_set_param(fd, &arg), -EPERM);
		}

		igt_waitchildren();
	}

	igt_describe("Verify that context set param ioctl works fine in root mode");
	igt_subtest("root-set") {
		arg.ctx_id = ctx;
		gem_context_get_param(fd, &arg);
		arg.value--;
		gem_context_set_param(fd, &arg);
	}

	igt_describe("Tests that multiple contexts can share the same VMA");
	igt_subtest("vm")
		test_vm(fd);

	arg.param = I915_CONTEXT_PARAM_PRIORITY;

	igt_describe("Verify that context set param ioctl returns relevant error if driver "
	       "doesn't supports assigning custom priorities from userspace");
	igt_subtest("set-priority-not-supported") {
		igt_require(!gem_scheduler_has_ctx_priority(fd));

		arg.ctx_id = ctx;
		arg.size = 0;

		igt_assert_eq(__gem_context_set_param(fd, &arg), -ENODEV);
	}

	igt_describe("Test performed with context param set to priority");
	igt_subtest_group {
		igt_fixture {
			igt_require(gem_scheduler_has_ctx_priority(fd));
		}

		igt_describe("Verify that priority is default for newly created context");
		igt_subtest("get-priority-new-ctx") {
			struct drm_i915_gem_context_param local_arg = arg;
			uint32_t local_ctx = gem_context_create(fd);

			local_arg.ctx_id = local_ctx;

			gem_context_get_param(fd, &local_arg);
			igt_assert_eq(local_arg.value, I915_CONTEXT_DEFAULT_PRIORITY);

			gem_context_destroy(fd, local_ctx);
		}

		igt_describe("Verify that relevant error is returned on setting invalid ctx size "
		       "with default priority");
		igt_subtest("set-priority-invalid-size") {
			struct drm_i915_gem_context_param local_arg = arg;
			local_arg.ctx_id = ctx;
			local_arg.value = 0;
			local_arg.size = ~0;

			igt_assert_eq(__gem_context_set_param(fd, &local_arg), -EINVAL);
		}

		igt_describe("Change priority range to test value overflow");
		igt_subtest("set-priority-range")
			set_priority(fd);
	}

	/* I915_CONTEXT_PARAM_SSEU tests are located in gem_ctx_sseu.c */

	arg.param = -1; /* Should be safely unused for a while */

	igt_describe("Checks that fetching context parameters using an unused param value "
	       "is erroneous");
	igt_subtest("invalid-param-get") {
		arg.ctx_id = ctx;
		igt_assert_eq(__gem_context_get_param(fd, &arg), -EINVAL);
	}

	igt_describe("Checks that setting context parameters using an unused param value "
	       "is erroneous");
	igt_subtest("invalid-param-set") {
		arg.ctx_id = ctx;
		igt_assert_eq(__gem_context_set_param(fd, &arg), -EINVAL);
	}

	igt_subtest("invalid-set-ringsize")
		test_set_invalid_param(fd, I915_CONTEXT_PARAM_RINGSIZE, 8192);

	igt_subtest("invalid-get-ringsize")
		test_get_invalid_param(fd, I915_CONTEXT_PARAM_RINGSIZE);

	igt_subtest("invalid-set-no-zeromap")
		test_set_invalid_param(fd, I915_CONTEXT_PARAM_NO_ZEROMAP, 1);

	igt_subtest("invalid-get-no-zeromap")
		test_get_invalid_param(fd, I915_CONTEXT_PARAM_NO_ZEROMAP);

	igt_subtest("invalid-get-engines")
		test_get_invalid_param(fd, I915_CONTEXT_PARAM_ENGINES);

	igt_fixture
		close(fd);
}
