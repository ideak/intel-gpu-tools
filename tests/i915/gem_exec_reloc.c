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

#include <signal.h>
#include <sys/ioctl.h>

#include "i915/gem.h"
#include "igt.h"
#include "igt_device.h"
#include "igt_dummyload.h"
#include "igt_kms.h"
#include "sw_sync.h"

IGT_TEST_DESCRIPTION("Basic sanity check of execbuf-ioctl relocations.");

#define ENGINE_MASK  (I915_EXEC_RING_MASK | I915_EXEC_BSD_MASK)

static uint32_t find_last_set(uint64_t x)
{
	uint32_t i = 0;
	while (x) {
		x >>= 1;
		i++;
	}
	return i;
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

static void write_dword(int fd,
			uint32_t target_handle,
			uint64_t target_offset,
			uint32_t value)
{
	unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc;
	uint32_t buf[16];
	int i;

	memset(obj, 0, sizeof(obj));
	obj[0].handle = target_handle;
	obj[1].handle = gem_create(fd, 4096);

	i = 0;
	buf[i++] = MI_STORE_DWORD_IMM | (gen < 6 ? 1<<22 : 0);
	if (gen >= 8) {
		buf[i++] = target_offset;
		buf[i++] = target_offset >> 32;
	} else if (gen >= 4) {
		buf[i++] = 0;
		buf[i++] = target_offset;
	} else {
		buf[i-1]--;
		buf[i++] = target_offset;
	}
	buf[i++] = value;
	buf[i++] = MI_BATCH_BUFFER_END;
	gem_write(fd, obj[1].handle, 0, buf, sizeof(buf));

	memset(&reloc, 0, sizeof(reloc));
	if (gen >= 8 || gen < 4)
		reloc.offset = sizeof(uint32_t);
	else
		reloc.offset = 2*sizeof(uint32_t);
	reloc.target_handle = target_handle;
	reloc.delta = target_offset;
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = I915_GEM_DOMAIN_INSTRUCTION;

	obj[1].relocation_count = 1;
	obj[1].relocs_ptr = to_user_pointer(&reloc);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	execbuf.flags = I915_EXEC_SECURE;
	gem_execbuf(fd, &execbuf);
	gem_close(fd, obj[1].handle);
}

enum mode { MEM, CPU, WC, GTT };
#define RO 0x100
static void from_mmap(int fd, uint64_t size, enum mode mode)
{
	uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_relocation_entry *relocs;
	uint32_t reloc_handle;
	uint64_t value;
	uint64_t max, i;
	int retry = 2;

	if ((mode & ~RO) == GTT)
		gem_require_mappable_ggtt(fd);

	/* Worst case is that the kernel has to copy the entire incoming
	 * reloc[], so double the memory requirements.
	 */
	intel_require_memory(2, size, CHECK_RAM);

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	max = size / sizeof(*relocs);
	switch (mode & ~RO) {
	case MEM:
		relocs = mmap(0, size,
			      PROT_WRITE, MAP_PRIVATE | MAP_ANON,
			      -1, 0);
		igt_assert(relocs != (void *)-1);
		break;
	case GTT:
		reloc_handle = gem_create(fd, size);
		relocs = gem_mmap__gtt(fd, reloc_handle, size, PROT_WRITE);
		gem_set_domain(fd, reloc_handle,
				I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
		gem_close(fd, reloc_handle);
		break;
	case CPU:
		reloc_handle = gem_create(fd, size);
		relocs = gem_mmap__cpu(fd, reloc_handle, 0, size, PROT_WRITE);
		gem_set_domain(fd, reloc_handle,
			       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
		gem_close(fd, reloc_handle);
		break;
	case WC:
		reloc_handle = gem_create(fd, size);
		relocs = gem_mmap__wc(fd, reloc_handle, 0, size, PROT_WRITE);
		gem_set_domain(fd, reloc_handle,
			       I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);
		gem_close(fd, reloc_handle);
		break;
	}

	for (i = 0; i < max; i++) {
		relocs[i].target_handle = obj.handle;
		relocs[i].presumed_offset = ~0ull;
		relocs[i].offset = 1024;
		relocs[i].delta = i;
		relocs[i].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		relocs[i].write_domain = 0;
	}
	obj.relocation_count = max;
	obj.relocs_ptr = to_user_pointer(relocs);

	if (mode & RO)
		mprotect(relocs, size, PROT_READ);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	while (relocs[0].presumed_offset == ~0ull && retry--)
		gem_execbuf(fd, &execbuf);
	gem_read(fd, obj.handle, 1024, &value, sizeof(value));
	gem_close(fd, obj.handle);

	igt_assert_eq_u64(value, obj.offset + max - 1);
	if (relocs[0].presumed_offset != ~0ull) {
		for (i = 0; i < max; i++)
			igt_assert_eq_u64(relocs[i].presumed_offset,
					  obj.offset);
	}
	munmap(relocs, size);
}

static void from_gpu(int fd)
{
	uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_relocation_entry *relocs;
	uint32_t reloc_handle;
	uint64_t value;

	igt_require(gem_can_store_dword(fd, 0));

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	reloc_handle = gem_create(fd, 4096);
	write_dword(fd,
		    reloc_handle,
		    offsetof(struct drm_i915_gem_relocation_entry,
			     target_handle),
		    obj.handle);
	write_dword(fd,
		    reloc_handle,
		    offsetof(struct drm_i915_gem_relocation_entry,
			     offset),
		    1024);
	write_dword(fd,
		    reloc_handle,
		    offsetof(struct drm_i915_gem_relocation_entry,
			     read_domains),
		    I915_GEM_DOMAIN_INSTRUCTION);

	relocs = gem_mmap__cpu(fd, reloc_handle, 0, 4096, PROT_READ);
	gem_set_domain(fd, reloc_handle,
		       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
	gem_close(fd, reloc_handle);

	obj.relocation_count = 1;
	obj.relocs_ptr = to_user_pointer(relocs);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	gem_execbuf(fd, &execbuf);
	gem_read(fd, obj.handle, 1024, &value, sizeof(value));
	gem_close(fd, obj.handle);

	igt_assert_eq_u64(value, obj.offset);
	igt_assert_eq_u64(relocs->presumed_offset, obj.offset);
	munmap(relocs, 4096);
}

static void check_bo(int fd, uint32_t handle)
{
	uint32_t *map;
	int i;

	igt_debug("Verifying result\n");
	map = gem_mmap__cpu(fd, handle, 0, 4096, PROT_READ);
	gem_set_domain(fd, handle, I915_GEM_DOMAIN_CPU, 0);
	for (i = 0; i < 1024; i++)
		igt_assert_eq(map[i], i);
	munmap(map, 4096);
}

static void active(int fd, unsigned engine)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	unsigned engines[I915_EXEC_RING_MASK + 1];
	unsigned nengine;
	int pass;

	nengine = 0;
	if (engine == ALL_ENGINES) {
		const struct intel_execution_engine2 *e;

		__for_each_physical_engine(fd, e) {
			if (gem_class_can_store_dword(fd, e->class))
				engines[nengine++] = e->flags;
		}
	} else {
		engines[nengine++] = engine;
	}
	igt_require(nengine);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = gem_create(fd, 4096);
	obj[1].handle = gem_create(fd, 64*1024);
	obj[1].relocs_ptr = to_user_pointer(&reloc);
	obj[1].relocation_count = 1;

	memset(&reloc, 0, sizeof(reloc));
	reloc.offset = sizeof(uint32_t);
	reloc.target_handle = obj[0].handle;
	if (gen < 8 && gen >= 4)
		reloc.offset += sizeof(uint32_t);
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = I915_GEM_DOMAIN_INSTRUCTION;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	for (pass = 0; pass < 1024; pass++) {
		uint32_t batch[16];
		int i = 0;
		batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
		if (gen >= 8) {
			batch[++i] = 0;
			batch[++i] = 0;
		} else if (gen >= 4) {
			batch[++i] = 0;
			batch[++i] = 0;
		} else {
			batch[i]--;
			batch[++i] = 0;
		}
		batch[++i] = pass;
		batch[++i] = MI_BATCH_BUFFER_END;
		gem_write(fd, obj[1].handle, pass*sizeof(batch),
			  batch, sizeof(batch));
	}

	for (pass = 0; pass < 1024; pass++) {
		reloc.delta = 4*pass;
		reloc.presumed_offset = -1;
		execbuf.flags &= ~ENGINE_MASK;
		execbuf.flags |= engines[rand() % nengine];
		gem_execbuf(fd, &execbuf);
		execbuf.batch_start_offset += 64;
		reloc.offset += 64;
	}
	gem_close(fd, obj[1].handle);

	check_bo(fd, obj[0].handle);
	gem_close(fd, obj[0].handle);
}

static uint64_t many_relocs(unsigned long count, unsigned long *out)
{
	struct drm_i915_gem_relocation_entry *reloc;
	unsigned long sz;
	int i;

	sz = count * sizeof(*reloc);
	sz = ALIGN(sz, 4096);

	reloc = mmap(0, sz, PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	igt_assert(reloc != MAP_FAILED);
	for (i = 0; i < count; i++) {
		reloc[i].target_handle = 0;
		reloc[i].presumed_offset = ~0ull;
		reloc[i].offset = 8 * i;
		reloc[i].delta = 8 * i;
	}
	mprotect(reloc, sz, PROT_READ);

	*out = sz;
	return to_user_pointer(reloc);
}

static void __many_active(int i915, unsigned engine, unsigned long count)
{
	unsigned long reloc_sz;
	struct drm_i915_gem_exec_object2 obj[2] = {{
		.handle = gem_create(i915, count * sizeof(uint64_t)),
		.relocs_ptr = many_relocs(count, &reloc_sz),
		.relocation_count = count,
	}};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(obj),
		.buffer_count = ARRAY_SIZE(obj),
		.flags = engine | I915_EXEC_HANDLE_LUT,
	};
	igt_spin_t *spin;

	spin = __igt_spin_new(i915,
			      .engine = engine,
			      .dependency = obj[0].handle,
			      .flags = (IGT_SPIN_FENCE_OUT |
					IGT_SPIN_NO_PREEMPTION));
	obj[1] = spin->obj[1];
	gem_execbuf(i915, &execbuf);
	igt_assert_eq(sync_fence_status(spin->out_fence), 0);
	igt_spin_free(i915, spin);

	for (unsigned long i = 0; i < count; i++) {
		uint64_t addr;

		gem_read(i915, obj[0].handle, i * sizeof(addr),
			 &addr, sizeof(addr));

		igt_assert_eq_u64(addr, obj[0].offset + i * sizeof(addr));
	}

	munmap(from_user_pointer(obj[0].relocs_ptr), reloc_sz);
	gem_close(i915, obj[0].handle);
}

static void many_active(int i915, unsigned engine)
{
	const uint64_t max = 2048;
	unsigned long count = 256;

	igt_until_timeout(2) {
		uint64_t required, total;

		if (!__intel_check_memory(1, 8 * count, CHECK_RAM,
					  &required, &total))
			break;

		igt_debug("Testing count:%lu\n", count);
		__many_active(i915, engine, count);

		count <<= 1;
		if (count >= max)
			break;
	}
}

static void __wide_active(int i915, unsigned engine, unsigned long count)
{
	struct drm_i915_gem_relocation_entry *reloc =
		calloc(count, sizeof(*reloc));
	struct drm_i915_gem_exec_object2 *obj =
		calloc(count + 1, sizeof(*obj));
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(obj),
		.buffer_count = count + 1,
		.flags = engine | I915_EXEC_HANDLE_LUT,
	};
	igt_spin_t *spin;

	for (unsigned long i = 0; i < count; i++) {
		obj[i].handle = gem_create(i915, 4096);
		obj[i].flags = EXEC_OBJECT_WRITE;
		obj[i].flags |= EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	}

	spin = __igt_spin_new(i915,
			      .engine = engine,
			      .flags = (IGT_SPIN_FENCE_OUT |
					IGT_SPIN_NO_PREEMPTION));
	obj[count] = spin->obj[1];
	gem_execbuf(i915, &execbuf); /* mark all the objects as active */

	for (unsigned long i = 0; i < count; i++) {
		reloc[i].target_handle = i;
		reloc[i].presumed_offset = ~0ull;
		obj[i].relocs_ptr = to_user_pointer(&reloc[i]);
		obj[i].relocation_count = 1;
	}
	gem_execbuf(i915, &execbuf); /* relocation onto active objects */

	igt_assert_eq(sync_fence_status(spin->out_fence), 0);
	igt_spin_free(i915, spin);

	for (unsigned long i = 0; i < count; i++) {
		uint64_t addr;

		gem_read(i915, obj[i].handle, 0, &addr, sizeof(addr));
		igt_assert_eq_u64(addr, obj[i].offset);

		gem_close(i915, obj[i].handle);
	}
	free(obj);
	free(reloc);
}

static void wide_active(int i915, unsigned engine)
{
	const uint64_t max = gem_aperture_size(i915) / 4096 / 2;
	unsigned long count = 256;

	igt_until_timeout(2) {
		uint64_t required, total;

		if (!__intel_check_memory(count, 4096, CHECK_RAM,
					  &required, &total))
			break;

		igt_debug("Testing count:%lu\n", count);
		__wide_active(i915, engine, count);

		count <<= 1;
		if (count >= max)
			break;
	}
}

static unsigned int offset_in_page(void *addr)
{
	return (uintptr_t)addr & 4095;
}

static void active_spin(int fd, unsigned engine)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	igt_spin_t *spin;

	spin = igt_spin_new(fd,
			    .engine = engine,
			    .flags = IGT_SPIN_NO_PREEMPTION);

	memset(obj, 0, sizeof(obj));
	obj[0] = spin->obj[IGT_SPIN_BATCH];
	obj[0].relocs_ptr = to_user_pointer(&reloc);
	obj[0].relocation_count = 1;
	obj[1].handle = gem_create(fd, 4096);
	gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));

	memset(&reloc, 0, sizeof(reloc));
	reloc.presumed_offset = -1;
	reloc.offset = offset_in_page(spin->condition);
	reloc.target_handle = obj[0].handle;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	execbuf.flags = engine;

	gem_execbuf(fd, &execbuf);
	gem_close(fd, obj[1].handle);
	igt_assert_eq(*spin->condition, spin->cmd_precondition);

	igt_spin_end(spin);
	gem_sync(fd, spin->handle);

	igt_assert_eq(*spin->condition, obj[0].offset);
	igt_spin_free(fd, spin);
}

static void others_spin(int i915, unsigned engine)
{
	struct drm_i915_gem_relocation_entry reloc = {};
	struct drm_i915_gem_exec_object2 obj = {
		.relocs_ptr = to_user_pointer(&reloc),
		.relocation_count = 1,
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = engine,
	};
	const struct intel_execution_engine2 *e;
	igt_spin_t *spin = NULL;
	uint64_t addr;
	int fence;

	__for_each_physical_engine(i915, e) {
		if (e->flags == engine)
			continue;

		if (!spin) {
			spin = igt_spin_new(i915,
					    .engine = e->flags,
					    .flags = IGT_SPIN_FENCE_OUT);
			fence = dup(spin->out_fence);
		} else {
			int old_fence;

			spin->execbuf.flags &= ~I915_EXEC_RING_MASK;
			spin->execbuf.flags |= e->flags;
			gem_execbuf_wr(i915, &spin->execbuf);

			old_fence = fence;
			fence = sync_fence_merge(old_fence,
						 spin->execbuf.rsvd2 >> 32);
			close(spin->execbuf.rsvd2 >> 32);
			close(old_fence);
		}
	}
	igt_require(spin);

	/* All other engines are busy, let's relocate! */
	obj.handle = batch_create(i915);
	reloc.target_handle = obj.handle;
	reloc.presumed_offset = -1;
	reloc.offset = 64;
	gem_execbuf(i915, &execbuf);

	/* Verify the relocation took place */
	gem_read(i915, obj.handle, 64, &addr, sizeof(addr));
	igt_assert_eq_u64(addr, obj.offset);
	gem_close(i915, obj.handle);

	/* Even if the spinner was harmed in the process */
	igt_spin_end(spin);
	igt_assert_eq(sync_fence_wait(fence, 200), 0);
	igt_assert_neq(sync_fence_status(fence), 0);
	if (sync_fence_status(fence) < 0)
		igt_warn("Spinner was cancelled, %s\n",
			 strerror(-sync_fence_status(fence)));
	close(fence);

	igt_spin_free(i915, spin);
}

static bool has_64b_reloc(int fd)
{
	return intel_gen(intel_get_drm_devid(fd)) >= 8;
}

#define NORELOC 1
#define ACTIVE 2
#define INTERRUPTIBLE 4
#define HANG 8
static void basic_reloc(int fd, unsigned before, unsigned after, unsigned flags)
{
#define OBJSZ 8192
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint64_t address_mask = has_64b_reloc(fd) ? ~(uint64_t)0 : ~(uint32_t)0;
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	unsigned int reloc_offset;

	if ((before | after) & I915_GEM_DOMAIN_GTT)
		gem_require_mappable_ggtt(fd);

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, OBJSZ);
	obj.relocs_ptr = to_user_pointer(&reloc);
	obj.relocation_count = 1;
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	if (flags & NORELOC)
		execbuf.flags |= I915_EXEC_NO_RELOC;

	for (reloc_offset = 4096 - 8; reloc_offset <= 4096 + 8; reloc_offset += 4) {
		igt_spin_t *spin = NULL;
		uint32_t trash = 0;
		uint64_t offset;

		obj.offset = -1;

		memset(&reloc, 0, sizeof(reloc));
		reloc.offset = reloc_offset;
		reloc.target_handle = obj.handle;
		reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc.presumed_offset = -1;

		if (before) {
			char *wc;

			if (before == I915_GEM_DOMAIN_CPU)
				wc = gem_mmap__cpu(fd, obj.handle, 0, OBJSZ, PROT_WRITE);
			else if (before == I915_GEM_DOMAIN_GTT)
				wc = gem_mmap__gtt(fd, obj.handle, OBJSZ, PROT_WRITE);
			else if (before == I915_GEM_DOMAIN_WC)
				wc = gem_mmap__wc(fd, obj.handle, 0, OBJSZ, PROT_WRITE);
			else
				igt_assert(0);
			gem_set_domain(fd, obj.handle, before, before);
			offset = -1;
			memcpy(wc + reloc_offset, &offset, sizeof(offset));
			munmap(wc, OBJSZ);
		} else {
			offset = -1;
			gem_write(fd, obj.handle, reloc_offset, &offset, sizeof(offset));
		}

		if (flags & ACTIVE) {
			spin = igt_spin_new(fd,
					    .engine = I915_EXEC_DEFAULT,
					    .dependency = obj.handle);
			if (!(flags & HANG))
				igt_spin_set_timeout(spin, NSEC_PER_SEC/100);
			igt_assert(gem_bo_busy(fd, obj.handle));
		}

		gem_execbuf(fd, &execbuf);

		if (after) {
			char *wc;

			if (after == I915_GEM_DOMAIN_CPU)
				wc = gem_mmap__cpu(fd, obj.handle, 0, OBJSZ, PROT_READ);
			else if (after == I915_GEM_DOMAIN_GTT)
				wc = gem_mmap__gtt(fd, obj.handle, OBJSZ, PROT_READ);
			else if (after == I915_GEM_DOMAIN_WC)
				wc = gem_mmap__wc(fd, obj.handle, 0, OBJSZ, PROT_READ);
			else
				igt_assert(0);
			gem_set_domain(fd, obj.handle, after, 0);
			offset = ~reloc.presumed_offset & address_mask;
			memcpy(&offset, wc + reloc_offset, has_64b_reloc(fd) ? 8 : 4);
			munmap(wc, OBJSZ);
		} else {
			offset = ~reloc.presumed_offset & address_mask;
			gem_read(fd, obj.handle, reloc_offset, &offset, has_64b_reloc(fd) ? 8 : 4);
		}

		if (reloc.presumed_offset == -1)
			igt_warn("reloc.presumed_offset == -1\n");
		else
			igt_assert_eq_u64(reloc.presumed_offset, offset);
		igt_assert_eq_u64(obj.offset, offset);

		igt_spin_free(fd, spin);

		/* Simulate relocation */
		if (flags & NORELOC) {
			obj.offset += OBJSZ;
			reloc.presumed_offset += OBJSZ;
		} else {
			trash = obj.handle;
			obj.handle = gem_create(fd, OBJSZ);
			gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));
			reloc.target_handle = obj.handle;
		}

		if (before) {
			char *wc;

			if (before == I915_GEM_DOMAIN_CPU)
				wc = gem_mmap__cpu(fd, obj.handle, 0, OBJSZ, PROT_WRITE);
			else if (before == I915_GEM_DOMAIN_GTT)
				wc = gem_mmap__gtt(fd, obj.handle, OBJSZ, PROT_WRITE);
			else if (before == I915_GEM_DOMAIN_WC)
				wc = gem_mmap__wc(fd, obj.handle, 0, OBJSZ, PROT_WRITE);
			else
				igt_assert(0);
			gem_set_domain(fd, obj.handle, before, before);
			memcpy(wc + reloc_offset, &reloc.presumed_offset, sizeof(reloc.presumed_offset));
			munmap(wc, OBJSZ);
		} else {
			gem_write(fd, obj.handle, reloc_offset, &reloc.presumed_offset, sizeof(reloc.presumed_offset));
		}

		if (flags & ACTIVE) {
			spin = igt_spin_new(fd,
					    .engine = I915_EXEC_DEFAULT,
					    .dependency = obj.handle);
			if (!(flags & HANG))
				igt_spin_set_timeout(spin, NSEC_PER_SEC/100);
			igt_assert(gem_bo_busy(fd, obj.handle));
		}

		gem_execbuf(fd, &execbuf);

		if (after) {
			char *wc;

			if (after == I915_GEM_DOMAIN_CPU)
				wc = gem_mmap__cpu(fd, obj.handle, 0, OBJSZ, PROT_READ);
			else if (after == I915_GEM_DOMAIN_GTT)
				wc = gem_mmap__gtt(fd, obj.handle, OBJSZ, PROT_READ);
			else if (after == I915_GEM_DOMAIN_WC)
				wc = gem_mmap__wc(fd, obj.handle, 0, OBJSZ, PROT_READ);
			else
				igt_assert(0);
			gem_set_domain(fd, obj.handle, after, 0);
			offset = ~reloc.presumed_offset & address_mask;
			memcpy(&offset, wc + reloc_offset, has_64b_reloc(fd) ? 8 : 4);
			munmap(wc, OBJSZ);
		} else {
			offset = ~reloc.presumed_offset & address_mask;
			gem_read(fd, obj.handle, reloc_offset, &offset, has_64b_reloc(fd) ? 8 : 4);
		}

		if (reloc.presumed_offset == -1)
			igt_warn("reloc.presumed_offset == -1\n");
		else
			igt_assert_eq_u64(reloc.presumed_offset, offset);
		igt_assert_eq_u64(obj.offset, offset);

		igt_spin_free(fd, spin);
		if (trash)
			gem_close(fd, trash);
	}

	gem_close(fd, obj.handle);
}

static inline uint64_t sign_extend(uint64_t x, int index)
{
	int shift = 63 - index;
	return (int64_t)(x << shift) >> shift;
}

static uint64_t gen8_canonical_address(uint64_t address)
{
	return sign_extend(address, 47);
}

static void basic_range(int fd, unsigned flags)
{
	struct drm_i915_gem_relocation_entry reloc[128];
	struct drm_i915_gem_exec_object2 obj[128];
	struct drm_i915_gem_execbuffer2 execbuf;
	uint64_t address_mask = has_64b_reloc(fd) ? ~(uint64_t)0 : ~(uint32_t)0;
	uint64_t gtt_size = gem_aperture_size(fd);
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	igt_spin_t *spin = NULL;
	int count, n;

	igt_require(gem_has_softpin(fd));

	for (count = 12; gtt_size >> (count + 1); count++)
		;

	count -= 12;

	memset(obj, 0, sizeof(obj));
	memset(reloc, 0, sizeof(reloc));
	memset(&execbuf, 0, sizeof(execbuf));

	n = 0;
	for (int i = 0; i <= count; i++) {
		obj[n].handle = gem_create(fd, 4096);
		obj[n].offset = (1ull << (i + 12)) - 4096;
		obj[n].offset = gen8_canonical_address(obj[n].offset);
		obj[n].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
		gem_write(fd, obj[n].handle, 0, &bbe, sizeof(bbe));
		execbuf.buffers_ptr = to_user_pointer(&obj[n]);
		execbuf.buffer_count = 1;
		if (__gem_execbuf(fd, &execbuf))
			continue;

		igt_debug("obj[%d] handle=%d, address=%llx\n",
			  n, obj[n].handle, (long long)obj[n].offset);

		reloc[n].offset = 8 * (n + 1);
		reloc[n].target_handle = obj[n].handle;
		reloc[n].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc[n].presumed_offset = -1;
		n++;
	}
	for (int i = 1; i < count; i++) {
		obj[n].handle = gem_create(fd, 4096);
		obj[n].offset = 1ull << (i + 12);
		obj[n].offset = gen8_canonical_address(obj[n].offset);
		obj[n].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
		gem_write(fd, obj[n].handle, 0, &bbe, sizeof(bbe));
		execbuf.buffers_ptr = to_user_pointer(&obj[n]);
		execbuf.buffer_count = 1;
		if (__gem_execbuf(fd, &execbuf))
			continue;

		igt_debug("obj[%d] handle=%d, address=%llx\n",
			  n, obj[n].handle, (long long)obj[n].offset);

		reloc[n].offset = 8 * (n + 1);
		reloc[n].target_handle = obj[n].handle;
		reloc[n].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc[n].presumed_offset = -1;
		n++;
	}
	igt_require(n);

	obj[n].handle = gem_create(fd, 4096);
	obj[n].relocs_ptr = to_user_pointer(reloc);
	obj[n].relocation_count = n;
	gem_write(fd, obj[n].handle, 0, &bbe, sizeof(bbe));

	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = n + 1;

	if (flags & ACTIVE) {
		spin = igt_spin_new(fd, .dependency = obj[n].handle);
		if (!(flags & HANG))
			igt_spin_set_timeout(spin, NSEC_PER_SEC/100);
		igt_assert(gem_bo_busy(fd, obj[n].handle));
	}

	gem_execbuf(fd, &execbuf);
	igt_spin_free(fd, spin);

	for (int i = 0; i < n; i++) {
		uint64_t offset;

		offset = ~reloc[i].presumed_offset & address_mask;
		gem_read(fd, obj[n].handle, reloc[i].offset,
			 &offset, has_64b_reloc(fd) ? 8 : 4);

		igt_debug("obj[%d] handle=%d, offset=%llx, found=%llx, presumed=%llx\n",
			  i, obj[i].handle,
			  (long long)obj[i].offset,
			  (long long)offset,
			  (long long)reloc[i].presumed_offset);

		igt_assert_eq_u64(obj[i].offset, offset);
		if (reloc[i].presumed_offset == -1)
			igt_warn("reloc.presumed_offset == -1\n");
		else
			igt_assert_eq_u64(reloc[i].presumed_offset, offset);
	}

	for (int i = 0; i <= n; i++)
		gem_close(fd, obj[i].handle);
}

static void basic_softpin(int fd)
{
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	uint64_t offset;
	uint32_t bbe = MI_BATCH_BUFFER_END;

	igt_require(gem_has_softpin(fd));

	memset(obj, 0, sizeof(obj));
	obj[1].handle = gem_create(fd, 4096);
	gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj[1]);
	execbuf.buffer_count = 1;
	gem_execbuf(fd, &execbuf);

	offset = obj[1].offset;

	obj[0].handle = gem_create(fd, 4096);
	obj[0].offset = obj[1].offset;
	obj[0].flags = EXEC_OBJECT_PINNED;

	execbuf.buffers_ptr = to_user_pointer(&obj[0]);
	execbuf.buffer_count = 2;

	gem_execbuf(fd, &execbuf);
	igt_assert_eq_u64(obj[0].offset, offset);

	gem_close(fd, obj[0].handle);
	gem_close(fd, obj[1].handle);
}

static struct drm_i915_gem_relocation_entry *
parallel_relocs(int count, unsigned long *out)
{
	struct drm_i915_gem_relocation_entry *reloc;
	unsigned long sz;
	int i;

	sz = count * sizeof(*reloc);
	sz = ALIGN(sz, 4096);

	reloc = mmap(0, sz, PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	igt_assert(reloc != MAP_FAILED);
	for (i = 0; i < count; i++) {
		reloc[i].target_handle = 0;
		reloc[i].presumed_offset = ~0ull;
		reloc[i].offset = 8 * i;
		reloc[i].delta = i;
		reloc[i].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc[i].write_domain = 0;
	}
	mprotect(reloc, sz, PROT_READ);

	*out = sz;
	return reloc;
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

static int stop;
static void sighandler(int sig)
{
	stop = 1;
}

static void parallel_child(int i915,
			   const struct intel_execution_engine2 *engine,
			   struct drm_i915_gem_relocation_entry *reloc,
			   uint32_t common)
{
	igt_spin_t *spin = __igt_spin_new(i915, .engine = engine->flags);
	struct drm_i915_gem_exec_object2 reloc_target = {
		.handle = gem_create(i915, 32 * 1024 * 8),
		.relocation_count = 32 * 1024,
		.relocs_ptr = to_user_pointer(reloc),
	};
	struct drm_i915_gem_exec_object2 obj[3] = {
		reloc_target,
		{ .handle = common },
		spin->obj[1],
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(obj),
		.buffer_count = ARRAY_SIZE(obj),
		.flags = engine->flags | I915_EXEC_HANDLE_LUT,
	};
	struct sigaction act = {
		.sa_handler = sighandler,
	};
	unsigned long count = 0;

	sigaction(SIGINT, &act, NULL);
	while (!READ_ONCE(stop)) {
		int err = __execbuf(i915, &execbuf);
		if (err == -EINTR)
			break;

		igt_assert_eq(err, 0);
		count++;
	}

	igt_info("%s: count %lu\n", engine->name, count);
	igt_spin_free(i915, spin);
}

static void kill_children(int sig)
{
	signal(sig, SIG_IGN);
	kill(-getpgrp(), SIGINT);
	signal(sig, SIG_DFL);
}

static void parallel(int i915)
{
	const struct intel_execution_engine2 *e;
	struct drm_i915_gem_relocation_entry *reloc;
	uint32_t common = gem_create(i915, 4096);
	uint32_t batch = batch_create(i915);
	unsigned long reloc_sz;

	reloc = parallel_relocs(32 * 1024, &reloc_sz);

	stop = 0;
	__for_each_physical_engine(i915, e) {
		igt_fork(child, 1)
			parallel_child(i915, e, reloc, common);
	}
	sleep(2);

	if (gem_scheduler_has_preemption(i915)) {
		uint32_t ctx = gem_context_clone_with_engines(i915, 0);

		__for_each_physical_engine(i915, e) {
			struct drm_i915_gem_exec_object2 obj[2] = {
				{ .handle = common },
				{ .handle = batch },
			};
			struct drm_i915_gem_execbuffer2 execbuf = {
				.buffers_ptr = to_user_pointer(obj),
				.buffer_count = ARRAY_SIZE(obj),
				.flags = e->flags,
				.rsvd1 = ctx,
			};
			gem_execbuf(i915, &execbuf);
		}

		gem_context_destroy(i915, ctx);
	}
	gem_sync(i915, batch);
	gem_close(i915, batch);

	kill_children(SIGINT);
	igt_waitchildren();

	gem_close(i915, common);
	munmap(reloc, reloc_sz);
}

#define CONCURRENT 1024

static uint64_t concurrent_relocs(int i915, int idx, int count)
{
	struct drm_i915_gem_relocation_entry *reloc;
	const unsigned int gen = intel_gen(intel_get_drm_devid(i915));
	unsigned long sz;
	int offset;

	sz = count * sizeof(*reloc);
	sz = ALIGN(sz, 4096);

	reloc = mmap(0, sz, PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	igt_assert(reloc != MAP_FAILED);

	offset = 1;
	if (gen >= 4 && gen < 8)
		offset += 1;

	for (int n = 0; n < count; n++) {
		reloc[n].presumed_offset = ~0ull;
		reloc[n].offset = (4 * n + offset) * sizeof(uint32_t);
		reloc[n].delta = (count * idx + n) * sizeof(uint32_t);
	}
	mprotect(reloc, sz, PROT_READ);

	return to_user_pointer(reloc);
}

static int flags_to_index(const struct intel_execution_engine2 *e)
{
	return (e->flags & 63) | ((e->flags >> 13) & 3) << 4;
}

static void xchg_u32(void *array, unsigned i, unsigned j)
{
	uint32_t *u32 = array;
	uint32_t tmp = u32[i];
	u32[i] = u32[j];
	u32[j] = tmp;
}

static void concurrent_child(int i915,
			     const struct intel_execution_engine2 *e,
			     uint32_t *common, int num_common,
			     int in, int out)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(i915));
	int idx = flags_to_index(e);
	uint64_t relocs = concurrent_relocs(i915, idx, CONCURRENT);
	struct drm_i915_gem_exec_object2 obj[num_common + 2];
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(obj),
		.buffer_count = ARRAY_SIZE(obj),
		.flags = e->flags | I915_EXEC_HANDLE_LUT | (gen < 6 ? I915_EXEC_SECURE : 0),
	};
	uint32_t *batch = &obj[num_common + 1].handle;
	unsigned long count = 0;
	uint32_t *x;
	int err = 0;

	memset(obj, 0, sizeof(obj));
	obj[0].handle = gem_create(i915, 64 * CONCURRENT * 4);

	igt_permute_array(common, num_common, xchg_u32);
	for (int n = 1; n <= num_common; n++) {
		obj[n].handle = common[n - 1];
		obj[n].relocation_count = CONCURRENT;
		obj[n].relocs_ptr = relocs;
	}

	obj[num_common + 1].relocation_count = CONCURRENT;
	obj[num_common + 1].relocs_ptr = relocs;

	x = gem_mmap__device_coherent(i915, obj[0].handle,
				      0, 64 * CONCURRENT * 4, PROT_READ);
	x += idx * CONCURRENT;

	do {
		read(in, batch, sizeof(*batch));
		if (!*batch)
			break;

		gem_execbuf(i915, &execbuf);
		gem_sync(i915, *batch); /* write hazards lies */

		for (int n = 0; n < CONCURRENT; n++) {
			if (x[n] != *batch) {
				igt_warn("%s: Invalid store [bad reloc] found:%08x at index %d, expected %08x\n",
					 e->name, x[n], n, *batch);
				err = -EINVAL;
				break;
			}
		}

		write(out, &err, sizeof(err));
		count++;
	} while (err == 0);

	gem_close(i915, obj[0].handle);
	igt_info("%s: completed %ld cycles\n", e->name, count);
}

static uint32_t create_concurrent_batch(int i915, unsigned int count)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(i915));
	size_t sz = ALIGN(4 * (1 + 4 * count), 4096);
	uint32_t handle = gem_create(i915, sz);
	uint32_t *map, *cs;
	uint32_t cmd;

	cmd = MI_STORE_DWORD_IMM;
	if (gen < 6)
		cmd |= 1 << 22;
	if (gen < 4)
		cmd--;

	cs = map = gem_mmap__device_coherent(i915, handle, 0, sz, PROT_WRITE);
	for (int n = 0; n < count; n++) {
		*cs++ = cmd;
		*cs++ = 0;
		if (gen >= 4) {
			*cs++ = 0;
			*cs++ = handle;
		} else {
			*cs++ = handle;
			*cs++ = 0;
		}
	}
	*cs++ = MI_BATCH_BUFFER_END;
	munmap(map, sz);

	return handle;
}

static void concurrent(int i915, int num_common)
{
	const struct intel_execution_engine2 *e;
	int in[2], out[2];
	uint32_t common[16];
	int result = -1;
	uint32_t batch;
	int nchild;

	/*
	 * Exercise a few clients all trying to submit the same batch
	 * buffer writing to different locations. This exercises that the
	 * relocation handling within the gem_execbuf() ioctl is atomic
	 * with respect to the batch -- that is this call to execbuf only
	 * uses the relocations as supplied with the ioctl and does not
	 * use any of the conflicting relocations from the concurrent
	 * submissions.
	 */

	pipe(in);
	pipe(out);

	for (int n = 0; n < num_common; n++)
		common[n] = gem_create(i915, 4 * 4 * CONCURRENT);

	nchild = 0;
	__for_each_physical_engine(i915, e) {
		if (!gem_class_can_store_dword(i915, e->class))
			continue;

		igt_fork(child, 1)
			concurrent_child(i915, e,
					 common, num_common,
					 in[0], out[1]);

		if (++nchild == 64)
			break;
	}
	close(in[0]);
	close(out[1]);
	igt_require(nchild > 1);

	igt_until_timeout(5) {
		batch = create_concurrent_batch(i915, CONCURRENT);

		for (int n = 0; n < nchild; n++)
			write(in[1], &batch, sizeof(batch));

		for (int n = 0; n < nchild; n++) {
			result = -1;
			read(out[0], &result, sizeof(result));
			if (result < 0)
				break;
		}

		gem_close(i915, batch);
		if (result < 0)
			break;
	}

	batch = 0;
	for (int n = 0; n < nchild; n++)
		write(in[1], &batch, sizeof(batch));

	close(in[1]);
	close(out[0]);

	igt_waitchildren();

	for (int n = 0; n < num_common; n++)
		gem_close(i915, common[n]);

	igt_assert_eq(result, 0);
}

static uint32_t
pin_scanout(igt_display_t *dpy, igt_output_t *output, struct igt_fb *fb)
{
	drmModeModeInfoPtr mode = igt_output_get_mode(output);
	igt_plane_t *primary;

	igt_create_pattern_fb(dpy->drm_fd, mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888,
			      LOCAL_I915_FORMAT_MOD_X_TILED, fb);

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, fb);

	igt_display_commit2(dpy, COMMIT_LEGACY);

	return fb->gem_handle;
}

static void scanout(int i915,
		    igt_display_t *dpy,
		    const struct intel_execution_engine2 *e)
{
	struct drm_i915_gem_relocation_entry reloc = {};
	struct drm_i915_gem_exec_object2 obj[2] = {
		[1] = { .handle = batch_create(i915) },
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(obj),
		.buffer_count = 2,
		.flags = e->flags,
	};
	igt_output_t *output;
	struct igt_fb fb;
	uint64_t *map;

	igt_display_reset(dpy);

	output = igt_get_single_output_for_pipe(dpy, PIPE_A);
	igt_require(output);
	igt_output_set_pipe(output, PIPE_A);

	/*
	 * Find where the scanout is in our GTT; on !full-ppgtt this will be
	 * the actual GGTT address of the scanout.
	 */
	obj[0].handle = pin_scanout(dpy, output, &fb);
	gem_execbuf(i915, &execbuf);
	igt_info("Scanout GTT address: %#llx\n", obj[0].offset);

	/* Relocations should match the scanout address */
	reloc.target_handle = obj[0].handle;
	reloc.presumed_offset = -1;
	reloc.offset = 4000;
	obj[1].relocation_count = 1;
	obj[1].relocs_ptr = to_user_pointer(&reloc);
	gem_execbuf(i915, &execbuf);
	igt_info("Reloc address: %#llx\n", reloc.presumed_offset);
	igt_assert_eq_u64(reloc.presumed_offset, obj[0].offset);

	/* The address written into the batch should match the relocation */
	gem_sync(i915, obj[1].handle);
	map = gem_mmap__device_coherent(i915, obj[1].handle,
					0, 4096, PROT_WRITE);
	igt_assert_eq_u64(map[500], obj[0].offset);
	munmap(map, 4096);

	/* And finally softpinning with the scanout address should work */
	obj[0].flags |= EXEC_OBJECT_PINNED;
	obj[1].relocation_count = 0;
	gem_execbuf(i915, &execbuf);
	igt_assert_eq_u64(obj[0].offset, reloc.presumed_offset);

	gem_close(i915, obj[1].handle);
	igt_remove_fb(dpy->drm_fd, &fb);
}

#define I915_GEM_GPU_DOMAINS \
	(I915_GEM_DOMAIN_RENDER | \
	 I915_GEM_DOMAIN_SAMPLER | \
	 I915_GEM_DOMAIN_COMMAND | \
	 I915_GEM_DOMAIN_INSTRUCTION | \
	 I915_GEM_DOMAIN_VERTEX)

static void invalid_domains(int fd)
{
	static const struct bad_domain {
		uint32_t read_domains;
		uint32_t write_domains;
		int expected;
	} bd[] = {
		{ I915_GEM_DOMAIN_CPU, 0, -EINVAL },
		{ I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU, -EINVAL },
		{ I915_GEM_DOMAIN_GTT, 0, -EINVAL },
		{ I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT, -EINVAL },
		{
			I915_GEM_DOMAIN_RENDER | I915_GEM_DOMAIN_INSTRUCTION,
			I915_GEM_DOMAIN_RENDER | I915_GEM_DOMAIN_INSTRUCTION,
			-EINVAL
		},
		{
			~(I915_GEM_GPU_DOMAINS |
			  I915_GEM_DOMAIN_GTT |
			  I915_GEM_DOMAIN_CPU),
			0,
			-EINVAL
		},
		{ I915_GEM_DOMAIN_GTT << 1, I915_GEM_DOMAIN_GTT << 1, -EINVAL },
	};
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_exec_object2 obj[2] = {
		{ .handle = gem_create(fd, 4096) },
		{ .handle = gem_create(fd, 4096) },
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(obj),
		.buffer_count = 2,
	};

	gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));
	gem_execbuf(fd, &execbuf); /* verify the 2 objects are valid first */

	obj[1].relocation_count = 1;
	obj[1].relocs_ptr = to_user_pointer(&reloc);

	memset(&reloc, 0, sizeof(reloc));
	reloc.target_handle = obj[0].handle;
	gem_execbuf(fd, &execbuf); /* verify the reloc is valid */

	for (int i = 0; i < ARRAY_SIZE(bd); i++) {
		int err;

		reloc.read_domains = bd[i].read_domains;
		reloc.write_domain = bd[i].write_domains;
		err = __gem_execbuf(fd, &execbuf);
		igt_assert_f(err == bd[i].expected,
			     "[%d] Invalid .read_domains=%x, .write_domain=%x not reported; got %d, expected %d\n",
			     i,
			     bd[i].read_domains,
			     bd[i].write_domains,
			     err, bd[i].expected);
	}

	gem_close(fd, obj[1].handle);
	gem_close(fd, obj[0].handle);
}

igt_main
{
	const struct intel_execution_engine2 *e;
	const struct mode {
		const char *name;
		unsigned before, after;
	} modes[] = {
		{ "cpu", I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU },
		{ "gtt", I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT },
		{ "wc", I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC },
		{ "cpu-gtt", I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_GTT },
		{ "gtt-cpu", I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_CPU },
		{ "cpu-wc", I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_WC },
		{ "wc-cpu", I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_CPU },
		{ "gtt-wc", I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_WC },
		{ "wc-gtt", I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_GTT },
		{ "cpu-read", I915_GEM_DOMAIN_CPU, 0 },
		{ "gtt-read", I915_GEM_DOMAIN_GTT, 0 },
		{ "wc-read", I915_GEM_DOMAIN_WC, 0 },
		{ "write-cpu", 0, I915_GEM_DOMAIN_CPU },
		{ "write-gtt", 0, I915_GEM_DOMAIN_GTT },
		{ "write-wc", 0, I915_GEM_DOMAIN_WC },
		{ "write-read", 0, 0 },
		{ },
	}, *m;
	const struct flags {
		const char *name;
		unsigned flags;
		bool basic;
	} flags[] = {
		{ "", 0 , true},
		{ "-noreloc", NORELOC, true },
		{ "-active", ACTIVE, true },
		{ "-interruptible", ACTIVE | INTERRUPTIBLE },
		{ "-hang", ACTIVE | HANG },
		{ },
	}, *f;
	uint64_t size;
	int fd = -1;

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
		igt_require_gem(fd);
		/* Check if relocations supported by platform */
		igt_require(gem_has_relocations(fd));
	}

	for (f = flags; f->name; f++) {
		igt_hang_t hang;

		igt_subtest_group {
			igt_fixture {
				if (f->flags & HANG)
					hang = igt_allow_hang(fd, 0, 0);
			}

			for (m = modes; m->name; m++) {
				igt_subtest_f("%s%s%s",
					      f->basic ? "basic-" : "",
					      m->name,
					      f->name) {
					if ((m->before | m->after) & I915_GEM_DOMAIN_WC)
						igt_require(gem_mmap__has_wc(fd));
					igt_while_interruptible(f->flags & INTERRUPTIBLE)
						basic_reloc(fd, m->before, m->after, f->flags);
				}
			}

			if (!(f->flags & NORELOC)) {
				igt_subtest_f("%srange%s",
					      f->basic ? "basic-" : "", f->name) {
					igt_while_interruptible(f->flags & INTERRUPTIBLE)
						basic_range(fd, f->flags);
				}
			}

			igt_fixture {
				if (f->flags & HANG)
					igt_disallow_hang(fd, hang);
			}
		}
	}

	igt_subtest("basic-softpin")
		basic_softpin(fd);

	for (size = 4096; size <= 4ull*1024*1024*1024; size <<= 1) {
		igt_subtest_f("mmap-%u", find_last_set(size) - 1)
			from_mmap(fd, size, MEM);
		igt_subtest_f("readonly-%u", find_last_set(size) - 1)
			from_mmap(fd, size, MEM | RO);
		igt_subtest_f("cpu-%u", find_last_set(size) - 1)
			from_mmap(fd, size, CPU);
		igt_subtest_f("wc-%u", find_last_set(size) - 1) {
			igt_require(gem_mmap__has_wc(fd));
			from_mmap(fd, size, WC);
		}
		igt_subtest_f("gtt-%u", find_last_set(size) - 1)
			from_mmap(fd, size, GTT);
	}

	igt_subtest("gpu")
		from_gpu(fd);

	igt_subtest_with_dynamic("basic-active") {
		igt_dynamic("all")
			active(fd, ALL_ENGINES);

		__for_each_physical_engine(fd, e) {
			if (!gem_class_can_store_dword(fd, e->class))
				continue;

			igt_dynamic_f("%s", e->name)
				active(fd, e->flags);
		}
	}

	igt_subtest_with_dynamic("basic-spin") {
		__for_each_physical_engine(fd, e) {
			igt_dynamic_f("%s", e->name)
				active_spin(fd, e->flags);
		}
	}

	igt_subtest_with_dynamic("basic-spin-others") {
		__for_each_physical_engine(fd, e) {
			igt_dynamic_f("%s", e->name)
				others_spin(fd, e->flags);
		}
	}

	igt_subtest_with_dynamic("basic-many-active") {
		__for_each_physical_engine(fd, e) {
			igt_dynamic_f("%s", e->name)
				many_active(fd, e->flags);
		}
	}

	igt_subtest_with_dynamic("basic-wide-active") {
		__for_each_physical_engine(fd, e) {
			igt_dynamic_f("%s", e->name)
				wide_active(fd, e->flags);
		}
	}

	igt_subtest("basic-parallel")
		parallel(fd);

	igt_subtest("basic-concurrent0")
		concurrent(fd, 0);
	igt_subtest("basic-concurrent16")
		concurrent(fd, 16);

	igt_subtest("invalid-domains")
		invalid_domains(fd);

	igt_subtest_group {
		igt_display_t display = {
			.drm_fd = fd,
			.n_pipes = IGT_MAX_PIPES
		};

		igt_fixture {
			igt_device_set_master(fd);
			kmstest_set_vt_graphics_mode();
			igt_display_require(&display, fd);
		}

		igt_subtest_with_dynamic("basic-scanout") {
			__for_each_physical_engine(fd, e) {
				igt_dynamic_f("%s", e->name)
					scanout(fd, &display, e);
			}
		}

		igt_fixture
			igt_display_fini(&display);
	}

	igt_fixture
		close(fd);
}
