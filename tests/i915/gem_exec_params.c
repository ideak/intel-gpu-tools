/*
 * Copyright (c) 2014 Intel Corporation
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
 *    Daniel Vetter
 *
 */

#include <unistd.h>
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

#include "drm.h"
#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/gem_ring.h"
#include "igt.h"
#include "igt_device.h"
#include "sw_sync.h"

static bool has_exec_batch_first(int fd)
{
	int val = -1;
	struct drm_i915_getparam gp = {
		.param = 48,
		.value = &val,
	};
	ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp);
	return val > 0;
}

static bool has_resource_streamer(int fd)
{
	int val = -1;
	struct drm_i915_getparam gp = {
		.param = I915_PARAM_HAS_RESOURCE_STREAMER,
		.value = &val,
	};
	ioctl(fd, DRM_IOCTL_I915_GETPARAM , &gp);
	return val > 0;
}

static void test_batch_first(int fd)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[3];
	struct drm_i915_gem_relocation_entry reloc[2];
	uint32_t *map, value;
	int i;

	igt_require(gem_can_store_dword(fd, 0));
	igt_require(has_exec_batch_first(fd));

	memset(obj, 0, sizeof(obj));
	memset(reloc, 0, sizeof(reloc));

	obj[0].handle = gem_create(fd, 4096);
	obj[1].handle = gem_create(fd, 4096);
	obj[2].handle = gem_create(fd, 4096);

	reloc[0].target_handle = obj[1].handle;
	reloc[0].offset = sizeof(uint32_t);
	reloc[0].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc[0].write_domain = I915_GEM_DOMAIN_INSTRUCTION;
	obj[0].relocs_ptr = to_user_pointer(&reloc[0]);
	obj[0].relocation_count = 1;

	i = 0;
	map = gem_mmap__cpu(fd, obj[0].handle, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, obj[0].handle,
			I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
	map[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		map[++i] = 0;
		map[++i] = 0;
	} else if (gen >= 4) {
		map[++i] = 0;
		map[++i] = 0;
		reloc[0].offset += sizeof(uint32_t);
	} else {
		map[i]--;
		map[++i] = 0;
	}
	map[++i] = 1;
	map[++i] = MI_BATCH_BUFFER_END;
	munmap(map, 4096);

	reloc[1].target_handle = obj[1].handle;
	reloc[1].offset = sizeof(uint32_t);
	reloc[1].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc[1].write_domain = I915_GEM_DOMAIN_INSTRUCTION;
	obj[2].relocs_ptr = to_user_pointer(&reloc[1]);
	obj[2].relocation_count = 1;

	i = 0;
	map = gem_mmap__cpu(fd, obj[2].handle, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, obj[2].handle,
			I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
	map[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		map[++i] = 0;
		map[++i] = 0;
	} else if (gen >= 4) {
		map[++i] = 0;
		map[++i] = 0;
		reloc[1].offset += sizeof(uint32_t);
	} else {
		map[i]--;
		map[++i] = 0;
	}
	map[++i] = 2;
	map[++i] = MI_BATCH_BUFFER_END;
	munmap(map, 4096);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = ARRAY_SIZE(obj);
	if (gen > 3 && gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	/* Normal mode */
	gem_execbuf(fd, &execbuf);
	gem_read(fd, obj[1].handle, 0, &value, sizeof(value));
	igt_assert_eq_u32(value, 2);

	/* Batch first mode */
	execbuf.flags |= I915_EXEC_BATCH_FIRST;
	gem_execbuf(fd, &execbuf);
	gem_read(fd, obj[1].handle, 0, &value, sizeof(value));
	igt_assert_eq_u32(value, 1);

	gem_close(fd, obj[2].handle);
	gem_close(fd, obj[1].handle);
	gem_close(fd, obj[0].handle);
}

static int has_secure_batches(const int fd)
{
	int v = -1;
	drm_i915_getparam_t gp = {
		.param = I915_PARAM_HAS_SECURE_BATCHES,
		.value = &v,
	};

	drmIoctl(fd, DRM_IOCTL_I915_GETPARAM, &gp);

	return v > 0;
}

static bool has_submit_fence(int fd)
{
	int v = 0;
	struct drm_i915_getparam gp = {
		.param = I915_PARAM_HAS_EXEC_SUBMIT_FENCE,
		.value = &v,
	};

	ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp, sizeof(gp));
	errno = 0;

	return v > 0;
}

static uint32_t batch_create(int i915)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle;

	handle = gem_create(i915, 4096);
	gem_write(i915, handle, 0, &bbe, sizeof(bbe));

	return handle;
}

static void readonly(int i915)
{
	struct drm_i915_gem_execbuffer2 *execbuf;
	struct drm_i915_gem_exec_object2 exec = {
		batch_create(i915)
	};

	execbuf = mmap(NULL, 4096, PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	igt_assert(execbuf != MAP_FAILED);

	execbuf->buffers_ptr = to_user_pointer(&exec);
	execbuf->buffer_count = 1;
	igt_assert(mprotect(execbuf, 4096, PROT_READ) == 0);

	gem_execbuf(i915, execbuf);
	gem_close(i915, exec.handle);

	munmap(execbuf, 4096);
}

static void mmapped(int i915)
{
	uint32_t buf, handle;

	buf = gem_create(i915, 4096);
	handle = batch_create(i915);

	for_each_mmap_offset_type(i915, t) { /* repetitive! */
		struct drm_i915_gem_execbuffer2 *execbuf;
		struct drm_i915_gem_exec_object2 *exec;

		execbuf = __gem_mmap_offset(i915, buf, 0, 4096,
					    PROT_WRITE, t->type);
		if (!execbuf)
			continue;

		gem_set_domain(i915, buf, t->domain, t->domain);
		exec = (struct drm_i915_gem_exec_object2 *)(execbuf + 1);
		exec->handle = handle;

		execbuf->buffers_ptr = to_user_pointer(exec);
		execbuf->buffer_count = 1;

		gem_execbuf(i915, execbuf);

		munmap(execbuf, 4096);
	}
	gem_close(i915, handle);
	gem_close(i915, buf);
}

static uint32_t batch_create_size(int fd, uint64_t size)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle;

	handle = gem_create(fd, size);
	gem_write(fd, handle, 0, &bbe, sizeof(bbe));

	return handle;
}

static void __invalid_batch_start(int fd,
				  struct drm_i915_gem_execbuffer2 *execbuf,
				  uint32_t start_offset,
				  uint32_t batch_len)
{
	execbuf->batch_start_offset = start_offset;
	execbuf->batch_len = batch_len;
	igt_assert_eq(__gem_execbuf(fd, execbuf), -EINVAL);
}

static void test_invalid_batch_start(int fd)
{
	uint32_t size = 4096;
	struct drm_i915_gem_exec_object2 exec = {
		.handle = batch_create_size(fd, size),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&exec),
		.buffer_count = 1,
	};

	__invalid_batch_start(fd, &execbuf, 0, -1);
	__invalid_batch_start(fd, &execbuf, -1, 0);
	__invalid_batch_start(fd, &execbuf, -1, -1);
	__invalid_batch_start(fd, &execbuf, -1U & ~0x7, 0);
	__invalid_batch_start(fd, &execbuf, 0, -1U & ~0x7);
	__invalid_batch_start(fd, &execbuf, size, 0);
	__invalid_batch_start(fd, &execbuf, size, size);

	gem_sync(fd, exec.handle);
	gem_close(fd, exec.handle);
}

static void test_larger_than_life_batch(int fd)
{
	const struct intel_execution_engine2 *e;
	uint64_t size = 1ULL << 32; /* batch_len is __u32 as per the ABI */
	struct drm_i915_gem_exec_object2 exec = {
		.handle = batch_create_size(fd, size),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&exec),
		.buffer_count = 1,
	};

	/*
	 * batch_len seems like it can have different interaction depending on
	 * the engine and HW -- but we know that only if the GTT can be larger
	 * than 4G do we run into u32 issues, so we can safely restrict our
	 * checking to that subset of machines.
	 */
	igt_require(size < gem_aperture_size(fd));
	intel_require_memory(2, size, CHECK_RAM); /* batch + shadow */

	__for_each_physical_engine(fd, e) {
		/* Keep the batch_len implicit [0] */
		execbuf.flags = e->flags;

		/* non-48b objects are limited to the low (4G - 4K) */
		igt_assert_eq(__gem_execbuf(fd, &execbuf), -ENOSPC);

		exec.flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
		igt_assert_eq(__gem_execbuf(fd, &execbuf), 0);
		exec.flags = 0;
	}

	gem_sync(fd, exec.handle);
	gem_close(fd, exec.handle);
}

struct drm_i915_gem_execbuffer2 execbuf;
struct drm_i915_gem_exec_object2 gem_exec[1];
uint32_t batch[2] = {MI_BATCH_BUFFER_END};
uint32_t handle, devid;
int fd;

igt_main
{
	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);

		devid = intel_get_drm_devid(fd);

		handle = gem_create(fd, 4096);
		gem_write(fd, handle, 0, batch, sizeof(batch));

		gem_exec[0].handle = handle;
		gem_exec[0].relocation_count = 0;
		gem_exec[0].relocs_ptr = 0;
		gem_exec[0].alignment = 0;
		gem_exec[0].offset = 0;
		gem_exec[0].flags = 0;
		gem_exec[0].rsvd1 = 0;
		gem_exec[0].rsvd2 = 0;

		execbuf.buffers_ptr = to_user_pointer(gem_exec);
		execbuf.buffer_count = 1;
		execbuf.batch_start_offset = 0;
		execbuf.batch_len = 8;
		execbuf.cliprects_ptr = 0;
		execbuf.num_cliprects = 0;
		execbuf.DR1 = 0;
		execbuf.DR4 = 0;
		execbuf.flags = 0;
		i915_execbuffer2_set_context_id(execbuf, 0);
		execbuf.rsvd2 = 0;
	}

	igt_subtest("readonly")
		readonly(fd);

	igt_subtest("mmapped")
		mmapped(fd);

#define RUN_FAIL(expected_errno) do { \
		igt_assert_eq(__gem_execbuf(fd, &execbuf), -expected_errno); \
	} while(0)

	igt_subtest("no-bsd") {
		igt_require(!gem_has_bsd(fd));
		execbuf.flags = I915_EXEC_BSD;
		RUN_FAIL(EINVAL);
	}
	igt_subtest("no-blt") {
		igt_require(!gem_has_blt(fd));
		execbuf.flags = I915_EXEC_BLT;
		RUN_FAIL(EINVAL);
	}
	igt_subtest("no-vebox") {
		igt_require(!gem_has_vebox(fd));
		execbuf.flags = I915_EXEC_VEBOX;
		RUN_FAIL(EINVAL);
	}
	igt_subtest("invalid-ring") {
		execbuf.flags = I915_EXEC_RING_MASK;
		RUN_FAIL(EINVAL);
	}

	igt_subtest("invalid-ring2") {
		execbuf.flags = I915_EXEC_VEBOX+1;
		RUN_FAIL(EINVAL);
	}

	igt_subtest("invalid-bsd-ring") {
		int expected;

		expected = 0;
		if (gem_has_bsd2(fd) || !gem_has_bsd(fd))
			expected = -EINVAL;

		execbuf.flags = I915_EXEC_BSD | I915_EXEC_BSD_MASK;
		igt_assert_eq(__gem_execbuf(fd, &execbuf), expected);
	}

	igt_subtest("invalid-bsd1-flag-on-render") {
		execbuf.flags = I915_EXEC_RENDER | I915_EXEC_BSD_RING1;
		RUN_FAIL(EINVAL);
	}

	igt_subtest("invalid-bsd2-flag-on-render") {
		execbuf.flags = I915_EXEC_RENDER | I915_EXEC_BSD_RING2;
		RUN_FAIL(EINVAL);
	}

	igt_subtest("invalid-bsd1-flag-on-blt") {
		execbuf.flags = I915_EXEC_BLT | I915_EXEC_BSD_RING1;
		RUN_FAIL(EINVAL);
	}

	igt_subtest("invalid-bsd2-flag-on-blt") {
		execbuf.flags = I915_EXEC_BLT | I915_EXEC_BSD_RING2;
		RUN_FAIL(EINVAL);
	}

	igt_subtest("invalid-bsd1-flag-on-vebox") {
		igt_require(gem_has_vebox(fd));
		execbuf.flags = I915_EXEC_VEBOX | I915_EXEC_BSD_RING1;
		RUN_FAIL(EINVAL);
	}

	igt_subtest("invalid-bsd2-flag-on-vebox") {
		igt_require(gem_has_vebox(fd));
		execbuf.flags = I915_EXEC_VEBOX | I915_EXEC_BSD_RING2;
		RUN_FAIL(EINVAL);
	}

	igt_subtest("rel-constants-invalid-ring") {
		igt_require(gem_has_bsd(fd));
		execbuf.flags = I915_EXEC_BSD | I915_EXEC_CONSTANTS_ABSOLUTE;
		RUN_FAIL(EINVAL);
	}

	igt_subtest("rel-constants-invalid-rel-gen5") {
		igt_require(intel_gen(devid) > 5);
		execbuf.flags = I915_EXEC_RENDER | I915_EXEC_CONSTANTS_REL_SURFACE;
		RUN_FAIL(EINVAL);
	}

	igt_subtest("rel-constants-invalid") {
		execbuf.flags = I915_EXEC_RENDER | (I915_EXEC_CONSTANTS_REL_SURFACE+(1<<6));
		RUN_FAIL(EINVAL);
	}

	igt_subtest("sol-reset-invalid") {
		igt_require(gem_has_bsd(fd));
		execbuf.flags = I915_EXEC_BSD | I915_EXEC_GEN7_SOL_RESET;
		RUN_FAIL(EINVAL);
	}

	igt_subtest("sol-reset-not-gen7") {
		igt_require(intel_gen(devid) != 7);
		execbuf.flags = I915_EXEC_RENDER | I915_EXEC_GEN7_SOL_RESET;
		RUN_FAIL(EINVAL);
	}

	igt_subtest("secure-non-root") {
		igt_require(has_secure_batches(fd));

		igt_fork(child, 1) {
			igt_drop_root();

			execbuf.flags = I915_EXEC_RENDER | I915_EXEC_SECURE;
			RUN_FAIL(EPERM);
		}

		igt_waitchildren();
	}

	igt_subtest("secure-non-master") {
		igt_require(has_secure_batches(fd));

		/* Requires root privilege... */
		igt_require(__igt_device_set_master(fd) == 0);
		igt_device_drop_master(fd); /* ... to drop master! */

		execbuf.flags = I915_EXEC_RENDER | I915_EXEC_SECURE;
		RUN_FAIL(EPERM);

		igt_device_set_master(fd);
		gem_execbuf(fd, &execbuf);

		igt_device_drop_master(fd); /* Only needs temporary master */
	}

	/* HANDLE_LUT and NO_RELOC are already exercised by gem_exec_lut_handle,
	 * EXEC_FENCE_IN and EXEC_FENCE_OUT correct usage is tested by
	 * gem_exec_fence, invalid usage of EXEC_FENCE_IN is tested below. */

	igt_subtest("invalid-flag") {
		/* NOTE: This test intentionally exercise the next available
		 * flag. Don't "fix" this testcase without adding the required
		 * tests for the new flag first. */
		execbuf.flags = I915_EXEC_RENDER | (I915_EXEC_FENCE_ARRAY << 1);
		RUN_FAIL(EINVAL);
	}

	/* rsvd1 aka context id is already exercised  by gem_ctx_bad_exec */

	igt_subtest("cliprects-invalid") {
		igt_require(intel_gen(devid) >= 5);
		execbuf.flags = 0;
		execbuf.num_cliprects = 1;
		RUN_FAIL(EINVAL);
		execbuf.num_cliprects = 0;
	}

	igt_subtest("rs-invalid") {
		bool has_rs = has_resource_streamer(fd);

		for_each_ring(it, fd) {
			int expect = -EINVAL;
			if (has_rs &&
			    (eb_ring(it) == 0 ||
			     eb_ring(it) == I915_EXEC_RENDER))
				expect = 0;

			execbuf.flags =
				eb_ring(it) | I915_EXEC_RESOURCE_STREAMER;
			igt_assert_eq(__gem_execbuf(fd, &execbuf), expect);
		}
	}

	igt_subtest("invalid-fence-in") {
		igt_require(gem_has_exec_fence(fd));
		execbuf.flags = I915_EXEC_FENCE_IN;
		execbuf.rsvd2 = -1;
		RUN_FAIL(EINVAL);
		execbuf.rsvd2 = fd;
		RUN_FAIL(EINVAL);
	}

	igt_subtest("invalid-fence-in-submit") {
		int timeline;

		igt_require(gem_has_exec_fence(fd));
		igt_require(has_submit_fence(fd));

		timeline = sw_sync_timeline_create();
		execbuf.rsvd2 = sw_sync_timeline_create_fence(timeline, 1);

		execbuf.flags = I915_EXEC_FENCE_IN;
		gem_execbuf(fd, &execbuf);

		execbuf.flags = I915_EXEC_FENCE_SUBMIT;
		gem_execbuf(fd, &execbuf);

		execbuf.flags = I915_EXEC_FENCE_IN | I915_EXEC_FENCE_SUBMIT;
		RUN_FAIL(EINVAL);

		close(execbuf.rsvd2);
		close(timeline);

		gem_sync(fd, handle);
	}

	igt_subtest("rsvd2-dirt") {
		igt_require(!gem_has_exec_fence(fd));
		execbuf.flags = 0;
		execbuf.rsvd2 = 1;
		RUN_FAIL(EINVAL);
		execbuf.rsvd2 = 0;
	}

	igt_subtest("batch-first")
		test_batch_first(fd);

	igt_subtest("invalid-batch-start-offset")
		test_invalid_batch_start(fd);

	igt_subtest("larger-than-life-batch")
		test_larger_than_life_batch(fd);

#define DIRT(name) \
	igt_subtest(#name "-dirt") { \
		execbuf.flags = 0; \
		execbuf.name = 1; \
		RUN_FAIL(EINVAL); \
		execbuf.name = 0; \
	}

	DIRT(cliprects_ptr);
	DIRT(DR1);
	DIRT(DR4);
#undef DIRT

#undef RUN_FAIL

	igt_fixture {
		gem_close(fd, handle);

		close(fd);
	}
}
