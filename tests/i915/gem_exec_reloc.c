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

#include "igt.h"
#include "igt_dummyload.h"
#include "sw_sync.h"

IGT_TEST_DESCRIPTION("Basic sanity check of execbuf-ioctl relocations.");

#define LOCAL_I915_EXEC_BSD_SHIFT      (13)
#define LOCAL_I915_EXEC_BSD_MASK       (3 << LOCAL_I915_EXEC_BSD_SHIFT)

#define LOCAL_I915_EXEC_NO_RELOC (1<<11)
#define LOCAL_I915_EXEC_HANDLE_LUT (1<<12)

#define ENGINE_MASK  (I915_EXEC_RING_MASK | LOCAL_I915_EXEC_BSD_MASK)

static uint32_t find_last_set(uint64_t x)
{
	uint32_t i = 0;
	while (x) {
		x >>= 1;
		i++;
	}
	return i;
}

static void write_dword(int fd,
			uint32_t target_handle,
			uint64_t target_offset,
			uint32_t value)
{
	int gen = intel_gen(intel_get_drm_devid(fd));
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
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	unsigned engines[16];
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
	const uint64_t max = gem_aperture_size(i915) / 2;
	unsigned long count = 256;

	igt_until_timeout(2) {
		uint64_t required, total;

		if (!__intel_check_memory(1, 8 * count, CHECK_RAM,
					  &required, &total))
			break;

		igt_debug("Testing count:%lu\n", count);
		__many_active(i915, engine, count);

		count <<= 1;
		if (count * 8 >= max)
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
		execbuf.flags |= LOCAL_I915_EXEC_NO_RELOC;

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

	igt_fixture
		close(fd);
}
