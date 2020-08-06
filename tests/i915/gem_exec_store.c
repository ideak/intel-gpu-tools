/*
 * Copyright © 2009 Intel Corporation
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

/** @file gem_exec_store.c
 *
 * Simplest non-NOOP only batch with verification.
 */

#include <strings.h>

#include "i915/gem.h"
#include "igt.h"
#include "igt_device.h"
#include "igt_gt.h"

#define ENGINE_MASK  (I915_EXEC_RING_MASK | I915_EXEC_BSD_MASK)

static void store_dword(int fd, const struct intel_execution_engine2 *e)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t batch[16];
	int i;

	intel_detect_and_clear_missed_interrupts(fd);
	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	execbuf.flags = e->flags;
	if (gen > 3 && gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	memset(obj, 0, sizeof(obj));
	obj[0].handle = gem_create(fd, 4096);
	obj[1].handle = gem_create(fd, 4096);

	memset(&reloc, 0, sizeof(reloc));
	reloc.target_handle = obj[0].handle;
	reloc.presumed_offset = 0;
	reloc.offset = sizeof(uint32_t);
	reloc.delta = 0;
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = I915_GEM_DOMAIN_INSTRUCTION;
	obj[1].relocs_ptr = to_user_pointer(&reloc);
	obj[1].relocation_count = 1;

	i = 0;
	batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		batch[++i] = 0;
		batch[++i] = 0;
	} else if (gen >= 4) {
		batch[++i] = 0;
		batch[++i] = 0;
		reloc.offset += sizeof(uint32_t);
	} else {
		batch[i]--;
		batch[++i] = 0;
	}
	batch[++i] = 0xc0ffee;
	batch[++i] = MI_BATCH_BUFFER_END;
	gem_write(fd, obj[1].handle, 0, batch, sizeof(batch));
	gem_execbuf(fd, &execbuf);
	gem_close(fd, obj[1].handle);

	gem_read(fd, obj[0].handle, 0, batch, sizeof(batch));
	gem_close(fd, obj[0].handle);
	igt_assert_eq(*batch, 0xc0ffee);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

#define PAGES 1
static void store_cachelines(int fd, const struct intel_execution_engine2 *e,
			     unsigned int flags)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 *obj;
	struct drm_i915_gem_relocation_entry *reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
#define NCACHELINES (4096/64)
	uint32_t *batch;
	int i;

	reloc = calloc(NCACHELINES, sizeof(*reloc));
	igt_assert(reloc);

	intel_detect_and_clear_missed_interrupts(fd);
	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffer_count = flags & PAGES ? NCACHELINES + 1 : 2;
	execbuf.flags = e->flags;
	if (gen > 3 && gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	obj = calloc(execbuf.buffer_count, sizeof(*obj));
	igt_assert(obj);
	for (i = 0; i < execbuf.buffer_count; i++)
		obj[i].handle = gem_create(fd, 4096);
	obj[i-1].relocs_ptr = to_user_pointer(reloc);
	obj[i-1].relocation_count = NCACHELINES;
	execbuf.buffers_ptr = to_user_pointer(obj);

	batch = gem_mmap__cpu(fd, obj[i-1].handle, 0, 4096, PROT_WRITE);

	i = 0;
	for (unsigned n = 0; n < NCACHELINES; n++) {
		reloc[n].target_handle = obj[n % (execbuf.buffer_count-1)].handle;
		reloc[n].presumed_offset = -1;
		reloc[n].offset = (i + 1)*sizeof(uint32_t);
		reloc[n].delta = 4 * (n * 16 + n % 16);
		reloc[n].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc[n].write_domain = I915_GEM_DOMAIN_INSTRUCTION;

		batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
		if (gen >= 8) {
			batch[++i] = 0;
			batch[++i] = 0;
		} else if (gen >= 4) {
			batch[++i] = 0;
			batch[++i] = 0;
			reloc[n].offset += sizeof(uint32_t);
		} else {
			batch[i]--;
			batch[++i] = 0;
		}
		batch[++i] = n | ~n << 16;
		i++;
	}
	batch[i++] = MI_BATCH_BUFFER_END;
	igt_assert(i < 4096 / sizeof(*batch));
	munmap(batch, 4096);
	gem_execbuf(fd, &execbuf);

	for (unsigned n = 0; n < NCACHELINES; n++) {
		uint32_t result;

		gem_read(fd, reloc[n].target_handle, reloc[n].delta,
			 &result, sizeof(result));

		igt_assert_eq_u32(result, n | ~n << 16);
	}
	free(reloc);

	for (unsigned n = 0; n < execbuf.buffer_count; n++)
		gem_close(fd, obj[n].handle);
	free(obj);

	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void store_all(int fd)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj[2];
	struct intel_execution_engine2 *engine;
	struct drm_i915_gem_relocation_entry *reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	unsigned *engines, *permuted;
	uint32_t batch[16];
	uint64_t offset;
	unsigned nengine;
	int value;
	int i, j;

	nengine = 0;
	__for_each_physical_engine(fd, engine) {
		if (!gem_class_can_store_dword(fd, engine->class))
			continue;
		nengine++;
	}
	igt_require(nengine);

	reloc = calloc(2*nengine, sizeof(*reloc));
	igt_assert(reloc);

	engines = calloc(nengine, sizeof(*engines));
	igt_assert(engines);

	permuted = calloc(nengine, sizeof(*permuted));
	igt_assert(permuted);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	memset(obj, 0, sizeof(obj));
	obj[0].handle = gem_create(fd, nengine*sizeof(uint32_t));
	obj[1].handle = gem_create(fd, 2*nengine*sizeof(batch));
	obj[1].relocation_count = 1;

	offset = sizeof(uint32_t);
	i = 0;
	batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		batch[++i] = 0;
		batch[++i] = 0;
	} else if (gen >= 4) {
		batch[++i] = 0;
		batch[++i] = 0;
		offset += sizeof(uint32_t);
	} else {
		batch[i]--;
		batch[++i] = 0;
	}
	batch[value = ++i] = 0xc0ffee;
	batch[++i] = MI_BATCH_BUFFER_END;

	nengine = 0;
	intel_detect_and_clear_missed_interrupts(fd);
	__for_each_physical_engine(fd, engine) {
		if (!gem_class_can_store_dword(fd, engine->class))
			continue;

		execbuf.flags &= ~ENGINE_MASK;
		execbuf.flags |= engine->flags;

		j = 2*nengine;
		reloc[j].target_handle = obj[0].handle;
		reloc[j].presumed_offset = ~0;
		reloc[j].offset = j*sizeof(batch) + offset;
		reloc[j].delta = nengine*sizeof(uint32_t);
		reloc[j].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc[j].write_domain = I915_GEM_DOMAIN_INSTRUCTION;
		obj[1].relocs_ptr = to_user_pointer(&reloc[j]);

		batch[value] = 0xdeadbeef;
		gem_write(fd, obj[1].handle, j*sizeof(batch),
			  batch, sizeof(batch));
		execbuf.batch_start_offset = j*sizeof(batch);
		gem_execbuf(fd, &execbuf);

		j = 2*nengine + 1;
		reloc[j].target_handle = obj[0].handle;
		reloc[j].presumed_offset = ~0;
		reloc[j].offset = j*sizeof(batch) + offset;
		reloc[j].delta = nengine*sizeof(uint32_t);
		reloc[j].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc[j].write_domain = I915_GEM_DOMAIN_INSTRUCTION;
		obj[1].relocs_ptr = to_user_pointer(&reloc[j]);

		batch[value] = nengine;
		gem_write(fd, obj[1].handle, j*sizeof(batch),
			  batch, sizeof(batch));
		execbuf.batch_start_offset = j*sizeof(batch);
		gem_execbuf(fd, &execbuf);

		engines[nengine++] = engine->flags;
	}
	gem_sync(fd, obj[1].handle);

	for (i = 0; i < nengine; i++) {
		obj[1].relocs_ptr = to_user_pointer(&reloc[2*i]);
		execbuf.batch_start_offset = 2*i*sizeof(batch);
		memcpy(permuted, engines, nengine*sizeof(engines[0]));
		igt_permute_array(permuted, nengine, igt_exchange_int);
		for (j = 0; j < nengine; j++) {
			execbuf.flags &= ~ENGINE_MASK;
			execbuf.flags |= permuted[j];
			gem_execbuf(fd, &execbuf);
		}
		obj[1].relocs_ptr = to_user_pointer(&reloc[2*i+1]);
		execbuf.batch_start_offset = (2*i+1)*sizeof(batch);
		execbuf.flags &= ~ENGINE_MASK;
		execbuf.flags |= engines[i];
		gem_execbuf(fd, &execbuf);
	}
	gem_close(fd, obj[1].handle);

	gem_read(fd, obj[0].handle, 0, engines, nengine*sizeof(engines[0]));
	gem_close(fd, obj[0].handle);

	for (i = 0; i < nengine; i++)
		igt_assert_eq_u32(engines[i], i);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);

	free(permuted);
	free(engines);
	free(reloc);
}

static int print_welcome(int fd)
{
	uint16_t devid = intel_get_drm_devid(fd);
	const struct intel_device_info *info = intel_get_device_info(devid);
	int err;

	igt_info("Running on %s (pci-id %04x, gen %d)\n",
		 info->codename, devid, ffs(info->gen));
	igt_info("Can use MI_STORE_DWORD(virtual)? %s\n",
		 gem_can_store_dword(fd, 0) ? "yes" : "no");

	err = 0;
	if (drmIoctl(fd, DRM_IOCTL_I915_GEM_THROTTLE, 0))
		err = -errno;
	igt_info("GPU operation? %s [errno=%d]\n",
		 err == 0 ? "yes" : "no", err);

	return ffs(info->gen);
}

#define test_each_engine(T, i915, e)  \
	igt_subtest_with_dynamic(T) __for_each_physical_engine(i915, e) \
		for_each_if(gem_class_can_store_dword(i915, (e)->class)) \
			igt_dynamic_f("%s", (e)->name)

igt_main
{
	const struct intel_execution_engine2 *e;
	int fd;

	igt_fixture {
		int gen;

		fd = drm_open_driver(DRIVER_INTEL);

		gen = print_welcome(fd);
		if (gen > 3 && gen < 6) /* ctg and ilk need secure batches */
			igt_device_set_master(fd);

		igt_require_gem(fd);

		igt_fork_hang_detector(fd);
	}

	igt_subtest("basic")
		store_all(fd);

	test_each_engine("dword", fd, e)
		store_dword(fd, e);

	test_each_engine("cachelines", fd, e)
		store_cachelines(fd, e, 0);

	test_each_engine("pages", fd, e)
		store_cachelines(fd, e, PAGES);

	igt_fixture {
		igt_stop_hang_detector();
		close(fd);
	}
}
