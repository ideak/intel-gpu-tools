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
 *    Vinay Belgaumkar <vinay.belgaumkar@intel.com>
 *    Thomas Daniel <thomas.daniel@intel.com>
 *
 */

#include "i915/gem.h"
#include "igt.h"
#include "intel_allocator.h"

#define EXEC_OBJECT_PINNED	(1<<4)
#define EXEC_OBJECT_SUPPORTS_48B_ADDRESS (1<<3)

#define LIMIT_32b ((1ull << 32) - (1ull << 12))

/* gen8_canonical_addr
 * Used to convert any address into canonical form, i.e. [63:48] == [47].
 * Based on kernel's sign_extend64 implementation.
 * @address - a virtual address
*/
#define GEN8_HIGH_ADDRESS_BIT 47
static uint64_t gen8_canonical_addr(uint64_t address)
{
	__u8 shift = 63 - GEN8_HIGH_ADDRESS_BIT;
	return (__s64)(address << shift) >> shift;
}

#define INTERRUPTIBLE 0x1

static void test_invalid(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 object;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&object);
	execbuf.buffer_count = 1;

	memset(&object, 0, sizeof(object));
	object.handle = gem_create(fd, 2*4096);
	object.flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS | EXEC_OBJECT_PINNED;
	gem_write(fd, object.handle, 0, &bbe, sizeof(bbe));

	/* Check invalid alignment */
	object.offset = 4096;
	object.alignment = 64*1024;
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EINVAL);
	object.alignment = 0;

	/* Check wraparound */
	object.offset = -4096ULL;
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EINVAL);

	/* Check beyond bounds of aperture */
	object.offset = gem_aperture_size(fd) - 4096;
	object.offset = gen8_canonical_addr(object.offset);
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EINVAL);

	/* Check gen8 canonical addressing */
	if (gem_aperture_size(fd) > 1ull<<GEN8_HIGH_ADDRESS_BIT) {
		object.offset = 1ull << GEN8_HIGH_ADDRESS_BIT;
		igt_assert_eq(__gem_execbuf(fd, &execbuf), -EINVAL);

		object.offset = gen8_canonical_addr(object.offset);
		igt_assert_eq(__gem_execbuf(fd, &execbuf), 0);
	}

	/* Check extended range */
	if (gem_aperture_size(fd) > 1ull<<32) {
		object.flags = EXEC_OBJECT_PINNED;
		object.offset = 1ull<<32;
		igt_assert_eq(__gem_execbuf(fd, &execbuf), -EINVAL);

		object.offset = gen8_canonical_addr(object.offset);
		object.flags |= EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
		igt_assert_eq(__gem_execbuf(fd, &execbuf), 0);
	}
}

static uint32_t batch_create(int i915, uint64_t *sz)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle;

	igt_assert_eq(__gem_create(i915, sz, &handle), 0);
	gem_write(i915, handle, 0, &bbe, sizeof(bbe));

	return handle;
}

static void test_zero(int i915)
{
	uint64_t sz = 4096, gtt = gem_aperture_size(i915);
	struct drm_i915_gem_exec_object2 object = {
		.handle = batch_create(i915, &sz),
		.flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS,
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&object),
		.buffer_count = 1,
	};

	igt_info("Object size:%"PRIx64", GTT size:%"PRIx64"\n", sz, gtt);

	object.offset = 0;
	igt_assert_f(__gem_execbuf(i915, &execbuf) == 0,
		     "execbuf failed with object.offset=%llx\n",
		     object.offset);

	if (gtt >> 32) {
		object.offset = (1ull << 32) - sz;
		igt_assert_f(__gem_execbuf(i915, &execbuf) == 0,
			     "execbuf failed with object.offset=%llx\n",
			     object.offset);
	}

	if ((gtt - sz) >> 32) {
		object.offset = 1ull << 32;
		igt_assert_f(__gem_execbuf(i915, &execbuf) == 0,
			     "execbuf failed with object.offset=%llx\n",
			     object.offset);
	}

	object.offset = gtt - sz;
	object.offset = gen8_canonical_addr(object.offset);
	igt_assert_f(__gem_execbuf(i915, &execbuf) == 0,
		     "execbuf failed with object.offset=%llx\n",
		     object.offset);

	gem_close(i915, object.handle);
}

static void test_32b_last_page(int i915)
{
	uint64_t sz = 4096, gtt = gem_aperture_size(i915);
	struct drm_i915_gem_exec_object2 object = {
		.flags = EXEC_OBJECT_PINNED,
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&object),
		.buffer_count = 1,
	};

	/*
	 * The last page under 32b is excluded for !48b objects in order to
	 * prevent issues with stateless addressing.
	 */

	igt_require(gtt >= 1ull << 32);
	object.handle = batch_create(i915, &sz),

	object.offset = 1ull << 32;
	object.offset -= sz;
	igt_assert_f(__gem_execbuf(i915, &execbuf) == -EINVAL,
		     "execbuf succeeded with object.offset=%llx + %"PRIx64"\n",
		     object.offset, sz);

	object.offset -= 4096;
	igt_assert_f(__gem_execbuf(i915, &execbuf) == 0,
		     "execbuf failed with object.offset=%llx + %"PRIx64"\n",
		     object.offset, sz);

	gem_close(i915, object.handle);
}

static void test_full(int i915)
{
	uint64_t sz = 4096, gtt = gem_aperture_size(i915);
	struct drm_i915_gem_exec_object2 obj[2] = {
		/* Use two objects so we can test .pad_to_size works */
		{
			.handle = batch_create(i915, &sz),
			.flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_PAD_TO_SIZE,
		},
		{
			.handle = batch_create(i915, &sz),
			.flags = EXEC_OBJECT_PINNED,
		},
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(obj),
		.buffer_count = ARRAY_SIZE(obj),
	};
	int err;

	obj[0].pad_to_size = gtt - sz;
	if (obj[0].pad_to_size > LIMIT_32b - sz)
		obj[0].pad_to_size = LIMIT_32b - sz;

	obj[1].offset = sz;
	igt_assert_f((err = __gem_execbuf(i915, &execbuf)) == -ENOSPC,
		     "[32b] execbuf succeeded with obj[1].offset=%llx and obj[0].pad_to_size=%llx: err=%d\n",
		     obj[1].offset, obj[0].pad_to_size, err);

	obj[1].offset = obj[0].pad_to_size;
	igt_assert_f((err = __gem_execbuf(i915, &execbuf)) == 0,
		     "[32b] execbuf failed with obj[1].offset=%llx and obj[0].pad_to_size=%llx: err=%d\n",
		     obj[1].offset, obj[0].pad_to_size, err);

	igt_assert_eq_u64(obj[0].offset, 0);
	igt_assert_eq_u64(obj[1].offset, obj[0].pad_to_size);

	if (obj[1].offset + sz < gtt) {
		obj[0].flags |= EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
		obj[1].flags |= EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

		obj[0].pad_to_size = gtt - sz;

		obj[1].offset = gen8_canonical_addr(obj[0].pad_to_size - sz);
		igt_assert_f((err = __gem_execbuf(i915, &execbuf)) == -ENOSPC,
			     "[48b] execbuf succeeded with obj[1].offset=%llx and obj[0].pad_to_size=%llx: err=%d\n",
			     obj[1].offset, obj[0].pad_to_size, err);

		obj[1].offset = gen8_canonical_addr(obj[0].pad_to_size);
		igt_assert_f((err = __gem_execbuf(i915, &execbuf)) == 0,
			     "[48b] execbuf failed with obj[1].offset=%llx and obj[0].pad_to_size=%llx: err=%d\n",
		     obj[1].offset, obj[0].pad_to_size, err);

		igt_assert_eq_u64(obj[0].offset, 0);
		igt_assert_eq_u64(obj[1].offset,
				  gen8_canonical_addr(obj[0].pad_to_size));
	}

	gem_close(i915, obj[1].handle);
	gem_close(i915, obj[0].handle);
}

static void test_softpin(int fd)
{
	const uint32_t size = 1024 * 1024;
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 object;
	uint64_t offset, end;
	uint32_t last_handle;
	unsigned long count = 0;

	last_handle = gem_create(fd, size);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&object);
	execbuf.buffer_count = 1;
	igt_until_timeout(30) {
		memset(&object, 0, sizeof(object));
		object.handle = gem_create(fd, 2*size);
		gem_write(fd, object.handle, 0, &bbe, sizeof(bbe));

		/* Find a hole */
		gem_execbuf(fd, &execbuf);
		gem_close(fd, object.handle);
		gem_close(fd, last_handle);

		igt_debug("Made a 2 MiB hole: %08llx\n",
			  object.offset);

		object.handle = gem_create(fd, size);
		gem_write(fd, object.handle, 0, &bbe, sizeof(bbe));
		object.flags |= EXEC_OBJECT_PINNED;

		end = object.offset + size;
		for (offset = object.offset; offset <= end; offset += 4096) {
			object.offset = offset;
			gem_execbuf(fd, &execbuf);
			igt_assert_eq_u64(object.offset, offset);
		}

		last_handle = object.handle;
		count++;
	}
	igt_info("Completed %lu cycles\n", count);

	gem_close(fd, last_handle);
}

static void invalid_execbuf(int i915, struct drm_i915_gem_execbuffer2 *execbuf)
{
	int err;

	/* More recent kernels do not track self-inflicted user errors */
	err = __gem_execbuf(i915, execbuf);
	igt_assert_f(err == -EINVAL || err == -ENOSPC,
		     "execbuf reported %d, not invalid (-EINVAL or -ENOSPC)\n",
		     err);
}

static void test_overlap(int fd)
{
	const uint32_t size = 1024 * 1024;
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 object[2];
	uint64_t offset;
	uint32_t handle;

	handle = gem_create(fd, 3*size);
	gem_write(fd, handle, 0, &bbe, sizeof(bbe));

	memset(object, 0, sizeof(object));
	object[0].handle = handle;

	/* Find a hole */
	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(object);
	execbuf.buffer_count = 1;
	gem_execbuf(fd, &execbuf);

	igt_debug("Made a 3x1 MiB hole: %08llx\n",
		  object[0].offset);

	object[0].handle = gem_create(fd, size);
	object[0].offset += size;
	object[0].flags |= EXEC_OBJECT_PINNED;
	object[1].handle = gem_create(fd, size);
	object[1].flags |= EXEC_OBJECT_PINNED;
	gem_write(fd, object[1].handle, 0, &bbe, sizeof(bbe));
	execbuf.buffer_count = 2;

	/* Check that we fit into our hole */
	object[1].offset = object[0].offset - size;
	gem_execbuf(fd, &execbuf);
	igt_assert_eq_u64(object[1].offset + size, object[0].offset);

	object[1].offset = object[0].offset + size;
	gem_execbuf(fd, &execbuf);
	igt_assert_eq_u64(object[1].offset - size, object[0].offset);

	/* Try all possible page-aligned overlaps */
	for (offset = object[0].offset - size + 4096;
	     offset < object[0].offset + size;
	     offset += 4096) {
		object[1].offset = offset;
		igt_debug("[0]=[%08llx - %08llx] [1]=[%08llx - %08llx]\n",
			  (long long)object[0].offset,
			  (long long)object[0].offset + size,
			  (long long)object[1].offset,
			  (long long)object[1].offset + size);

		invalid_execbuf(fd, &execbuf);
		igt_assert_eq_u64(object[1].offset, offset);
	}

	gem_close(fd, object[1].handle);
	gem_close(fd, object[0].handle);
	gem_close(fd, handle);
}

static void test_reverse(int i915)
{
	const uint32_t size = 1024 * 1024;
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 object[2];
	uint64_t offset;
	uint32_t handle;

	handle = gem_create(i915, 2 * size);
	gem_write(i915, handle, 0, &bbe, sizeof(bbe));

	memset(object, 0, sizeof(object));
	object[0].handle = handle;

	/* Find a hole */
	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(object);
	execbuf.buffer_count = 1;
	gem_execbuf(i915, &execbuf);

	igt_debug("Made a 2x1 MiB hole: %08llx\n", object[0].offset);
	offset = object[0].offset;

	object[0].handle = gem_create(i915, size);
	object[0].flags |= EXEC_OBJECT_PINNED;
	object[1].handle = gem_create(i915, size);
	object[1].flags |= EXEC_OBJECT_PINNED;
	gem_write(i915, object[1].handle, 0, &bbe, sizeof(bbe));
	execbuf.buffer_count = 2;

	/* Check that we fit into our hole */
	object[1].offset = offset + size;
	gem_execbuf(i915, &execbuf);
	igt_assert_eq_u64(object[0].offset, offset);
	igt_assert_eq_u64(object[1].offset, offset + size);

	/* And then swap over the placements */
	object[0].offset = offset + size;
	object[1].offset = offset;
	gem_execbuf(i915, &execbuf);
	igt_assert_eq_u64(object[1].offset, offset);
	igt_assert_eq_u64(object[0].offset, offset + size);

	gem_close(i915, object[1].handle);
	gem_close(i915, object[0].handle);
	gem_close(i915, handle);
}

static uint64_t busy_batch(int fd)
{
	unsigned const int gen = intel_gen(intel_get_drm_devid(fd));
	const int has_64bit_reloc = gen >= 8;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 object[2];
	uint32_t *map;
	int factor = 10;
	int i = 0;

	memset(object, 0, sizeof(object));
	object[0].handle = gem_create(fd, 1024*1024);
	object[1].handle = gem_create(fd, 4096);
	map = gem_mmap__cpu(fd, object[1].handle, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, object[1].handle,
		       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	*map = MI_BATCH_BUFFER_END;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(object);
	execbuf.buffer_count = 2;
	if (gen >= 6)
		execbuf.flags = I915_EXEC_BLT;
	gem_execbuf(fd, &execbuf);

	igt_debug("Active offsets = [%08llx, %08llx]\n",
		  object[0].offset, object[1].offset);

#define COPY_BLT_CMD		(2<<29|0x53<<22|0x6)
#define BLT_WRITE_ALPHA		(1<<21)
#define BLT_WRITE_RGB		(1<<20)
	gem_set_domain(fd, object[1].handle,
		       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
	while (factor--) {
		/* XY_SRC_COPY */
		map[i++] = COPY_BLT_CMD | BLT_WRITE_ALPHA | BLT_WRITE_RGB;
		if (has_64bit_reloc)
			map[i-1] += 2;
		map[i++] = 0xcc << 16 | 1 << 25 | 1 << 24 | (4*1024);
		map[i++] = 0;
		map[i++] = 256 << 16 | 1024;
		map[i++] = object[0].offset;
		if (has_64bit_reloc)
			map[i++] = object[0].offset >> 32;
		map[i++] = 0;
		map[i++] = 4096;
		map[i++] = object[0].offset;
		if (has_64bit_reloc)
			map[i++] = object[0].offset >> 32;
	}
	map[i++] = MI_BATCH_BUFFER_END;
	munmap(map, 4096);

	object[0].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_WRITE;
	object[1].flags = EXEC_OBJECT_PINNED;
	gem_execbuf(fd, &execbuf);
	gem_close(fd, object[0].handle);
	gem_close(fd, object[1].handle);

	return object[1].offset;
}

static void test_evict_active(int fd, unsigned int flags)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 object;
	uint64_t expected;

	memset(&object, 0, sizeof(object));
	object.handle = gem_create(fd, 4096);
	gem_write(fd, object.handle, 0, &bbe, sizeof(bbe));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&object);
	execbuf.buffer_count = 1;

	expected = busy_batch(fd);
	object.offset = expected;
	object.flags = EXEC_OBJECT_PINNED;

	/* Replace the active batch with ourselves, forcing an eviction */
	igt_while_interruptible(flags & INTERRUPTIBLE)
		gem_execbuf(fd, &execbuf);
	igt_assert_eq_u64(object.offset, expected);

	gem_close(fd, object.handle);
}

static void test_evict_snoop(int fd, unsigned int flags)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 object[2];
	uint64_t hole;

	igt_require(!gem_has_llc(fd));
	igt_require(!gem_uses_ppgtt(fd));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(object);
	execbuf.buffer_count = 1;

	/* Find a hole */
	memset(object, 0, sizeof(object));
	object[0].handle = gem_create(fd, 5*4096);
	gem_write(fd, object[0].handle, 0, &bbe, sizeof(bbe));
	gem_execbuf(fd, &execbuf);
	gem_close(fd, object[0].handle);
	hole = object[0].offset + 4096;

	/* Create a snoop + uncached pair */
	object[0].handle = gem_create(fd, 4096);
	object[0].flags = EXEC_OBJECT_PINNED;
	gem_set_caching(fd, object[0].handle, 1);
	object[1].handle = gem_create(fd, 4096);
	object[1].flags = EXEC_OBJECT_PINNED;
	gem_write(fd, object[1].handle, 4096-sizeof(bbe), &bbe, sizeof(bbe));
	execbuf.buffer_count = 2;

	/* snoop abutting before uncached -> error */
	object[0].offset = hole;
	object[1].offset = hole + 4096;
	invalid_execbuf(fd, &execbuf);

	/* snoop abutting after uncached -> error */
	object[0].offset = hole + 4096;
	object[1].offset = hole;
	invalid_execbuf(fd, &execbuf);

	/* with gap -> okay */
	object[0].offset = hole + 2*4096;
	object[1].offset = hole;
	igt_while_interruptible(flags & INTERRUPTIBLE)
		gem_execbuf(fd, &execbuf);

	/* And we should force the snoop away (or the GPU may hang) */
	object[0].flags = 0;
	object[1].offset = hole + 4096;
	igt_while_interruptible(flags & INTERRUPTIBLE)
		gem_execbuf(fd, &execbuf);
	igt_assert(object[0].offset != hole);
	igt_assert(object[0].offset != hole + 2*4096);

	gem_close(fd, object[0].handle);
	gem_close(fd, object[1].handle);
}

static void test_evict_hang(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 object;
	igt_hang_t hang;
	uint64_t expected;

	memset(&object, 0, sizeof(object));
	object.handle = gem_create(fd, 4096);
	gem_write(fd, object.handle, 0, &bbe, sizeof(bbe));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&object);
	execbuf.buffer_count = 1;

	hang = igt_hang_ctx(fd, 0, 0, 0);
	expected = hang.spin->obj[IGT_SPIN_BATCH].offset;

	/* Replace the hung batch with ourselves, forcing an eviction */
	object.offset = expected;
	object.flags = EXEC_OBJECT_PINNED;
	gem_execbuf(fd, &execbuf);
	igt_assert_eq_u64(object.offset, expected);

	igt_post_hang_ring(fd, hang);
	gem_close(fd, object.handle);
}

static void xchg_offset(void *array, unsigned i, unsigned j)
{
	struct drm_i915_gem_exec_object2 *object = array;
	uint64_t tmp = object[i].offset;
	object[i].offset = object[j].offset;
	object[j].offset = tmp;
}

enum sleep { NOSLEEP, SUSPEND, HIBERNATE };
static void test_noreloc(int fd, enum sleep sleep, unsigned flags)
{
	unsigned const int gen = intel_gen(intel_get_drm_devid(fd));
	const uint32_t size = 4096;
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 object[257];
	uint64_t offset;
	uint32_t handle;
	uint32_t *batch, *b;
	int i, loop = 0;

	handle = gem_create(fd, (ARRAY_SIZE(object)+1)*size);
	gem_write(fd, handle, 0, &bbe, sizeof(bbe));

	memset(object, 0, sizeof(object));
	object[0].handle = handle;

	/* Find a hole */
	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(object);
	execbuf.buffer_count = 1;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;
	gem_execbuf(fd, &execbuf);
	gem_close(fd, object[0].handle);

	igt_debug("Made a %dx%d KiB hole: %08llx\n",
		  (int)ARRAY_SIZE(object), size/1024, object[0].offset);

	offset = object[0].offset;
	for (i = 0; i < ARRAY_SIZE(object) - 1; i++) {
		object[i].handle = gem_create(fd, size);
		object[i].offset = offset + i*size;
		object[i].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_WRITE;
	}
	object[i].handle = gem_create(fd, 2*size);
	object[i].offset = offset + i*size;
	object[i].flags = EXEC_OBJECT_PINNED;

	b = batch = gem_mmap__cpu(fd, object[i].handle, 0, 2*size, PROT_WRITE);
	gem_set_domain(fd, object[i].handle,
		       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
	for (i = 0; i < ARRAY_SIZE(object) - 1; i++) {
		*b++ = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
		if (gen >= 8) {
			*b++ = object[i].offset;
			*b++ = object[i].offset >> 32;
		} else if (gen >= 4) {
			*b++ = 0;
			*b++ = object[i].offset;
		} else {
			b[-1]--;
			*b++ = object[i].offset;
		}
		*b++ = i;
	}
	*b++ = MI_BATCH_BUFFER_END;
	igt_assert(b - batch <= 2*size/sizeof(uint32_t));
	munmap(batch, size);

	execbuf.buffer_count = ARRAY_SIZE(object);
	igt_until_timeout(5) {
		igt_permute_array(object, ARRAY_SIZE(object)-1, xchg_offset);

		igt_while_interruptible(flags & INTERRUPTIBLE)
			gem_execbuf(fd, &execbuf);

		if ((loop++ & 127) == 0) {
			switch (sleep) {
			case NOSLEEP:
				break;
			case SUSPEND:
				igt_system_suspend_autoresume(SUSPEND_STATE_MEM,
							      SUSPEND_TEST_NONE);
				break;
			case HIBERNATE:
				igt_system_suspend_autoresume(SUSPEND_STATE_DISK,
							      SUSPEND_TEST_NONE);
				break;
			}
		}

		for (i = 0; i < ARRAY_SIZE(object) - 1; i++) {
			uint32_t val;

			gem_read(fd, object[i].handle, 0, &val, sizeof(val));
			igt_assert_eq(val, (object[i].offset - offset)/size);
		}
	}

	for (i = 0; i < ARRAY_SIZE(object); i++)
		gem_close(fd, object[i].handle);
}

static void __reserve(uint64_t ahnd, int i915, bool pinned,
		      struct drm_i915_gem_exec_object2 *objects,
		      int num_obj, uint64_t size)
{
	uint64_t gtt = gem_aperture_size(i915);
	unsigned int flags;
	int i;

	igt_assert(num_obj > 1);

	flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	if (pinned)
		flags |= EXEC_OBJECT_PINNED;

	memset(objects, 0, sizeof(objects) * num_obj);

	for (i = 0; i < num_obj; i++) {
		objects[i].handle = gem_create(i915, size);
		if (i < num_obj/2)
			objects[i].offset = i * size;
		else
			objects[i].offset = gtt - (i + 1 - num_obj/2) * size;
		objects[i].flags = flags;

		intel_allocator_reserve(ahnd, objects[i].handle,
					size, objects[i].offset);
		igt_debug("Reserve i: %d, handle: %u, offset: %llx\n", i,
			  objects[i].handle, (long long) objects[i].offset);
	}
}

static void __unreserve(uint64_t ahnd, int i915,
			struct drm_i915_gem_exec_object2 *objects,
			int num_obj, uint64_t size)
{
	int i;

	for (i = 0; i < num_obj; i++) {
		intel_allocator_unreserve(ahnd, objects[i].handle,
					  size, objects[i].offset);
		igt_debug("Unreserve i: %d, handle: %u, offset: %llx\n", i,
			  objects[i].handle, (long long) objects[i].offset);
		gem_close(i915, objects[i].handle);
	}
}

static void __exec_using_allocator(uint64_t ahnd, int i915, int num_obj,
				   bool pinned)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 object[num_obj];
	uint64_t stored_offsets[num_obj];
	unsigned int flags;
	uint64_t sz = 4096;
	int i;

	igt_assert(num_obj > 10);

	flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	if (pinned)
		flags |= EXEC_OBJECT_PINNED;

	memset(object, 0, sizeof(object));

	for (i = 0; i < num_obj; i++) {
		sz = (rand() % 15 + 1) * 4096;
		if (i == num_obj - 1)
			sz = 4096;
		object[i].handle = gem_create(i915, sz);
		object[i].offset =
			intel_allocator_alloc(ahnd, object[i].handle, sz, 0);
	}
	gem_write(i915, object[--i].handle, 0, &bbe, sizeof(bbe));

	for (i = 0; i < num_obj; i++) {
		object[i].flags = flags;
		object[i].offset = gen8_canonical_addr(object[i].offset);
		stored_offsets[i] = object[i].offset;
	}

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(object);
	execbuf.buffer_count = num_obj;
	gem_execbuf(i915, &execbuf);

	for (i = 0; i < num_obj; i++) {
		igt_assert(intel_allocator_free(ahnd, object[i].handle));
		gem_close(i915, object[i].handle);
	}

	/* Check kernel will keep offsets even if pinned is not set. */
	for (i = 0; i < num_obj; i++)
		igt_assert_eq_u64(stored_offsets[i], object[i].offset);
}

static void test_allocator_basic(int fd, bool reserve)
{
	const int num_obj = 257, num_reserved = 8;
	struct drm_i915_gem_exec_object2 objects[num_reserved];
	uint64_t ahnd, ressize = 4096;

	/*
	 * Check that we can place objects at start/end
	 * of the GTT using the allocator.
	 */
	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_SIMPLE);

	if (reserve)
		__reserve(ahnd, fd, true, objects, num_reserved, ressize);
	__exec_using_allocator(ahnd, fd, num_obj, true);
	if (reserve)
		__unreserve(ahnd, fd, objects, num_reserved, ressize);
	igt_assert(intel_allocator_close(ahnd) == true);
}

static void test_allocator_nopin(int fd, bool reserve)
{
	const int num_obj = 257, num_reserved = 8;
	struct drm_i915_gem_exec_object2 objects[num_reserved];
	uint64_t ahnd, ressize = 4096;

	/*
	 * Check that we can combine manual placement with automatic
	 * GTT placement.
	 *
	 * This will also check that we agree with this small sampling of
	 * allocator placements -- that is the given the same restrictions
	 * in execobj[] the kernel does not reject the placement due
	 * to overlaps or invalid addresses.
	 */
	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_SIMPLE);

	if (reserve)
		__reserve(ahnd, fd, false, objects, num_reserved, ressize);

	__exec_using_allocator(ahnd, fd, num_obj, false);

	if (reserve)
		__unreserve(ahnd, fd, objects, num_reserved, ressize);

	igt_assert(intel_allocator_close(ahnd) == true);
}

static void test_allocator_fork(int fd)
{
	const int num_obj = 17, num_reserved = 8;
	struct drm_i915_gem_exec_object2 objects[num_reserved];
	uint64_t ahnd, ressize = 4096;

	/*
	 * Must be called before opening allocator in multiprocess environment
	 * due to freeing previous allocator infrastructure and proper setup
	 * of data structures and allocation thread.
	 */
	intel_allocator_multiprocess_start();

	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_SIMPLE);
	__reserve(ahnd, fd, true, objects, num_reserved, ressize);

	igt_fork(child, 8) {
		ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_SIMPLE);
		igt_until_timeout(2)
			__exec_using_allocator(ahnd, fd, num_obj, true);
		intel_allocator_close(ahnd);
	}

	igt_waitchildren();

	__unreserve(ahnd, fd, objects, num_reserved, ressize);
	igt_assert(intel_allocator_close(ahnd) == true);

	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_SIMPLE);
	igt_assert(intel_allocator_close(ahnd) == true);

	intel_allocator_multiprocess_stop();
}

igt_main
{
	int fd = -1;

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
		igt_require_gem(fd);
		gem_require_blitter(fd);
		igt_require(gem_has_softpin(fd));
		igt_require(gem_can_store_dword(fd, 0));
	}

	igt_subtest("invalid")
		test_invalid(fd);

	igt_subtest_group {
		/* Under full-ppgtt, we have complete control of the GTT */

		igt_fixture {
			igt_require(gem_uses_full_ppgtt(fd));
		}

		igt_subtest("zero")
			test_zero(fd);

		igt_subtest("32b-excludes-last-page")
			test_32b_last_page(fd);

		igt_subtest("full")
			test_full(fd);

		igt_subtest("allocator-basic")
			test_allocator_basic(fd, false);

		igt_subtest("allocator-basic-reserve")
			test_allocator_basic(fd, true);

		igt_subtest("allocator-nopin")
			test_allocator_nopin(fd, false);

		igt_subtest("allocator-nopin-reserve")
			test_allocator_nopin(fd, true);

		igt_subtest("allocator-fork")
			test_allocator_fork(fd);
	}

	igt_subtest("softpin")
		test_softpin(fd);
	igt_subtest("overlap")
		test_overlap(fd);
	igt_subtest("reverse")
		test_reverse(fd);

	igt_subtest("noreloc")
		test_noreloc(fd, NOSLEEP, 0);
	igt_subtest("noreloc-interruptible")
		test_noreloc(fd, NOSLEEP, INTERRUPTIBLE);
	igt_subtest("noreloc-S3")
		test_noreloc(fd, SUSPEND, 0);
	igt_subtest("noreloc-S4")
		test_noreloc(fd, HIBERNATE, 0);

	for (int signal = 0; signal <= 1; signal++) {
		igt_subtest_f("evict-active%s", signal ? "-interruptible" : "")
			test_evict_active(fd, signal);
		igt_subtest_f("evict-snoop%s", signal ? "-interruptible" : "")
			test_evict_snoop(fd, signal);
	}
	igt_subtest("evict-hang")
		test_evict_hang(fd);

	igt_fixture
		close(fd);
}
