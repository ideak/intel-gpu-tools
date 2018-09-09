/*
 * Copyright © 2016 Intel Corporation
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

#include "igt.h"
#include "igt_device.h"
#include "igt_sysfs.h"

#define LOCAL_OBJECT_CAPTURE (1 << 7)
#define LOCAL_PARAM_HAS_EXEC_CAPTURE 45

IGT_TEST_DESCRIPTION("Check that we capture the user specified objects on a hang");

static void check_error_state(int dir, struct drm_i915_gem_exec_object2 *obj)
{
	char *error, *str;
	bool found = false;

	error = igt_sysfs_get(dir, "error");
	igt_sysfs_set(dir, "error", "Begone!");

	igt_assert(error);
	igt_debug("%s\n", error);

	/* render ring --- user = 0x00000000 ffffd000 */
	for (str = error; (str = strstr(str, "--- user = ")); str++) {
		uint64_t addr;
		uint32_t hi, lo;

		igt_assert(sscanf(str, "--- user = 0x%x %x", &hi, &lo) == 2);
		addr = hi;
		addr <<= 32;
		addr |= lo;
		igt_assert_eq_u64(addr, obj->offset);
		found = true;
	}

	igt_assert(found);
}

static void __capture(int fd, int dir, unsigned ring, uint32_t target)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj[4];
#define SCRATCH 0
#define CAPTURE 1
#define NOCAPTURE 2
#define BATCH 3
	struct drm_i915_gem_relocation_entry reloc[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t *batch, *seqno;
	int i;

	memset(obj, 0, sizeof(obj));
	obj[SCRATCH].handle = gem_create(fd, 4096);
	obj[CAPTURE].handle = target;
	obj[CAPTURE].flags = LOCAL_OBJECT_CAPTURE;
	obj[NOCAPTURE].handle = gem_create(fd, 4096);

	obj[BATCH].handle = gem_create(fd, 4096);
	obj[BATCH].relocs_ptr = (uintptr_t)reloc;
	obj[BATCH].relocation_count = ARRAY_SIZE(reloc);

	memset(reloc, 0, sizeof(reloc));
	reloc[0].target_handle = obj[BATCH].handle; /* recurse */
	reloc[0].presumed_offset = 0;
	reloc[0].offset = 5*sizeof(uint32_t);
	reloc[0].delta = 0;
	reloc[0].read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc[0].write_domain = 0;

	reloc[1].target_handle = obj[SCRATCH].handle; /* breadcrumb */
	reloc[1].presumed_offset = 0;
	reloc[1].offset = sizeof(uint32_t);
	reloc[1].delta = 0;
	reloc[1].read_domains = I915_GEM_DOMAIN_RENDER;
	reloc[1].write_domain = I915_GEM_DOMAIN_RENDER;

	seqno = gem_mmap__wc(fd, obj[SCRATCH].handle, 0, 4096, PROT_READ);
	gem_set_domain(fd, obj[SCRATCH].handle,
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	batch = gem_mmap__cpu(fd, obj[BATCH].handle, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, obj[BATCH].handle,
			I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	i = 0;
	batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		batch[++i] = 0;
		batch[++i] = 0;
	} else if (gen >= 4) {
		batch[++i] = 0;
		batch[++i] = 0;
		reloc[1].offset += sizeof(uint32_t);
	} else {
		batch[i]--;
		batch[++i] = 0;
	}
	batch[++i] = 0xc0ffee;
	if (gen < 4)
		batch[++i] = MI_NOOP;

	batch[++i] = MI_BATCH_BUFFER_START; /* not crashed? try again! */
	if (gen >= 8) {
		batch[i] |= 1 << 8 | 1;
		batch[++i] = 0;
		batch[++i] = 0;
	} else if (gen >= 6) {
		batch[i] |= 1 << 8;
		batch[++i] = 0;
	} else {
		batch[i] |= 2 << 6;
		batch[++i] = 0;
		if (gen < 4) {
			batch[i] |= 1;
			reloc[0].delta = 1;
		}
	}
	munmap(batch, 4096);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)obj;
	execbuf.buffer_count = ARRAY_SIZE(obj);
	execbuf.flags = ring;
	if (gen > 3 && gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	igt_assert(!READ_ONCE(*seqno));
	gem_execbuf(fd, &execbuf);

	/* Wait for the request to start */
	while (READ_ONCE(*seqno) != 0xc0ffee)
		igt_assert(gem_bo_busy(fd, obj[SCRATCH].handle));
	munmap(seqno, 4096);

	/* Check that only the buffer we marked is reported in the error */
	igt_force_gpu_reset(fd);
	check_error_state(dir, &obj[CAPTURE]);

	gem_sync(fd, obj[BATCH].handle);

	gem_close(fd, obj[BATCH].handle);
	gem_close(fd, obj[NOCAPTURE].handle);
	gem_close(fd, obj[SCRATCH].handle);
}

static void capture(int fd, int dir, unsigned ring)
{
	uint32_t handle;

	handle = gem_create(fd, 4096);
	__capture(fd, dir, ring, handle);
	gem_close(fd, handle);
}

static void userptr(int fd, int dir)
{
	uint32_t handle;
	void *ptr;

	igt_assert(posix_memalign(&ptr, 4096, 4096) == 0);
	igt_require(__gem_userptr(fd, ptr, 4096, 0, 0, &handle) == 0);

	__capture(fd, dir, 0, handle);

	gem_close(fd, handle);
	free(ptr);
}

static bool has_capture(int fd)
{
	drm_i915_getparam_t gp;
	int async = -1;

	gp.param = LOCAL_PARAM_HAS_EXEC_CAPTURE;
	gp.value = &async;
	drmIoctl(fd, DRM_IOCTL_I915_GETPARAM, &gp);

	return async > 0;
}

igt_main
{
	const struct intel_execution_engine *e;
	igt_hang_t hang;
	int fd = -1;
	int dir = -1;

	igt_skip_on_simulation();

	igt_fixture {
		int gen;

		fd = drm_open_driver(DRIVER_INTEL);

		gen = intel_gen(intel_get_drm_devid(fd));
		if (gen > 3 && gen < 6) /* ctg and ilk need secure batches */
			igt_device_set_master(fd);

		igt_require_gem(fd);
		gem_require_mmap_wc(fd);
		igt_require(has_capture(fd));
		igt_allow_hang(fd, 0, HANG_ALLOW_CAPTURE);

		dir = igt_sysfs_open(fd, NULL);
		igt_require(igt_sysfs_set(dir, "error", "Begone!"));
	}

	for (e = intel_execution_engines; e->name; e++) {
		/* default exec-id is purely symbolic */
		if (e->exec_id == 0)
			continue;

		igt_subtest_f("capture-%s", e->name) {
			igt_require(gem_ring_has_physical_engine(fd, e->exec_id | e->flags));
			igt_require(gem_can_store_dword(fd, e->exec_id | e->flags));
			capture(fd, dir, e->exec_id | e->flags);
		}
	}

	/* And check we can read from different types of objects */

	igt_subtest_f("userptr") {
		igt_require(gem_can_store_dword(fd, 0));
		userptr(fd, dir);
	}

	igt_fixture {
		close(dir);
		igt_disallow_hang(fd, hang);
		close(fd);
	}
}
