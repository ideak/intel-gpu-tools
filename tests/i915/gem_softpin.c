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
#include "i915/gem_create.h"
#include "igt.h"
#include "igt_rand.h"
#include "intel_allocator.h"

IGT_TEST_DESCRIPTION("Tests softpin feature with normal usage, invalid inputs"
		     " scenarios and couple of eviction tests which copy buffers"
		     " between CPU and GPU.");

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
	uint64_t start, end;
	unsigned int flags;
	int i;

	igt_assert(num_obj > 1);

	flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	if (pinned)
		flags |= EXEC_OBJECT_PINNED;

	memset(objects, 0, sizeof(objects) * num_obj);
	intel_allocator_get_address_range(ahnd, &start, &end);

	for (i = 0; i < num_obj; i++) {
		objects[i].handle = gem_create(i915, size);
		if (i < num_obj/2)
			objects[i].offset = start + i * size;
		else
			objects[i].offset = end - (i + 1 - num_obj/2) * size;
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

#define BATCH_SIZE (4096<<10)
/* We don't have alignment detection yet, so assume the worst-case scenario. */
#define BATCH_ALIGNMENT (1 << 21)

struct batch {
	uint32_t handle;
	void *ptr;
};

static void xchg_batch(void *array, unsigned int i, unsigned int j)
{
	struct batch *batches = array;
	struct batch tmp;

	tmp = batches[i];
	batches[i] = batches[j];
	batches[j] = tmp;
}

static void submit(int fd, unsigned int gen,
		   struct drm_i915_gem_execbuffer2 *eb,
		   struct batch *batches, unsigned int count,
		   uint64_t ahnd)
{
	struct drm_i915_gem_exec_object2 obj;
	uint32_t batch[16];
	uint64_t address;
	unsigned n;

	memset(&obj, 0, sizeof(obj));
	obj.flags = EXEC_OBJECT_PINNED;

	for (unsigned i = 0; i < count; i++) {
		obj.handle = batches[i].handle;
		obj.offset = intel_allocator_alloc(ahnd, obj.handle,
						   BATCH_SIZE,
						   BATCH_ALIGNMENT);
		address = obj.offset + BATCH_SIZE - eb->batch_start_offset - 8;
		n = 0;
		batch[n] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
		if (gen >= 8) {
			batch[n] |= 1 << 21;
			batch[n]++;
			batch[++n] = address;
			batch[++n] = address >> 32;
		} else if (gen >= 4) {
			batch[++n] = 0;
			batch[++n] = address;
		} else {
			batch[n]--;
			batch[++n] = address;
		}
		batch[++n] = obj.offset; /* lower_32_bits(value) */
		batch[++n] = obj.offset >> 32; /* upper_32_bits(value) / nop */
		batch[++n] = MI_BATCH_BUFFER_END;
		eb->buffers_ptr = to_user_pointer(&obj);

		memcpy(batches[i].ptr + eb->batch_start_offset,
		       batch, sizeof(batch));

		gem_execbuf(fd, eb);
	}
	/* As we have been lying about the write_domain, we need to do a sync */
	gem_sync(fd, obj.handle);
}

static void test_allocator_evict(int fd, const intel_ctx_t *ctx,
				 unsigned ring, int timeout)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_execbuffer2 execbuf;
	unsigned engines[I915_EXEC_RING_MASK + 1];
	volatile uint64_t *shared;
	struct timespec tv = {};
	struct batch *batches;
	unsigned nengine;
	unsigned count;
	uint64_t size, ahnd;

	shared = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(shared != MAP_FAILED);

	nengine = 0;
	if (ring == ALL_ENGINES) {
		struct intel_execution_engine2 *e;

		for_each_ctx_engine(fd, ctx, e) {
			if (!gem_class_can_store_dword(fd, e->class))
				continue;

			engines[nengine++] = e->flags;
		}
	} else {
		engines[nengine++] = ring;
	}
	igt_require(nengine);
	igt_assert(nengine * 64 <= BATCH_SIZE);

	size = gem_aperture_size(fd);
	if (!gem_uses_full_ppgtt(fd))
		size /= 2;
	if (size > 1ull<<32) /* Limit to 4GiB as we do not use allow-48b */
		size = 1ull << 32;
	igt_require(size < (1ull<<32) * BATCH_SIZE);

	count = size / BATCH_SIZE + 1;
	igt_debug("Using %'d batches to fill %'llu aperture on %d engines\n",
		  count, (long long)size, nengine);

	intel_allocator_multiprocess_start();
	ahnd = intel_allocator_open_full(fd, 0, 0, size / 16,
					 INTEL_ALLOCATOR_RELOC,
					 ALLOC_STRATEGY_NONE, 0);

	igt_require_memory(count, BATCH_SIZE, CHECK_RAM);
	intel_detect_and_clear_missed_interrupts(fd);

	igt_nsec_elapsed(&tv);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffer_count = 1;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;
	execbuf.rsvd1 = ctx->id;

	batches = calloc(count, sizeof(*batches));
	igt_assert(batches);
	for (unsigned i = 0; i < count; i++) {
		batches[i].handle = gem_create(fd, BATCH_SIZE);
		batches[i].ptr =
			gem_mmap__device_coherent(fd, batches[i].handle,
						  0, BATCH_SIZE, PROT_WRITE);
	}

	/* Flush all memory before we start the timer */
	submit(fd, gen, &execbuf, batches, count, ahnd);

	igt_info("Setup %u batches in %.2fms\n",
		 count, 1e-6 * igt_nsec_elapsed(&tv));

	igt_fork(child, nengine) {
		uint64_t dst, src, dst_offset, src_offset;
		uint64_t cycles = 0;

		hars_petruska_f54_1_random_perturb(child);
		igt_permute_array(batches, count, xchg_batch);
		execbuf.batch_start_offset = child * 64;
		execbuf.flags |= engines[child];

		dst_offset = BATCH_SIZE - child*64 - 8;
		if (gen >= 8)
			src_offset = child*64 + 3*sizeof(uint32_t);
		else if (gen >= 4)
			src_offset = child*64 + 4*sizeof(uint32_t);
		else
			src_offset = child*64 + 2*sizeof(uint32_t);

		/* We need to open the allocator again in the new process */
		ahnd = intel_allocator_open_full(fd, 0, 0, size / 16,
						 INTEL_ALLOCATOR_RELOC,
						 ALLOC_STRATEGY_NONE, 0);

		igt_until_timeout(timeout) {
			submit(fd, gen, &execbuf, batches, count, ahnd);
			for (unsigned i = 0; i < count; i++) {
				dst = *(uint64_t *)(batches[i].ptr + dst_offset);
				src = *(uint64_t *)(batches[i].ptr + src_offset);
				igt_assert_eq_u64(dst, src);
			}
			cycles++;
		}
		shared[child] = cycles;
		igt_info("engine[%d]: %llu cycles\n", child, (long long)cycles);
		intel_allocator_close(ahnd);
	}
	igt_waitchildren();

	intel_allocator_close(ahnd);
	intel_allocator_multiprocess_stop();

	for (unsigned i = 0; i < count; i++) {
		munmap(batches[i].ptr, BATCH_SIZE);
		gem_close(fd, batches[i].handle);
	}
	free(batches);

	shared[nengine] = 0;
	for (unsigned i = 0; i < nengine; i++)
		shared[nengine] += shared[i];
	igt_info("Total: %llu cycles\n", (long long)shared[nengine]);

	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

#define MINIMAL_OFFSET 0x200000
static void single_offset_submit(int fd, struct drm_i915_gem_execbuffer2 *eb,
				 struct batch *batches, unsigned int count)
{
	struct drm_i915_gem_exec_object2 obj = {
		.offset = max_t(uint64_t, gem_detect_safe_start_offset(fd), MINIMAL_OFFSET),
		.flags = EXEC_OBJECT_PINNED,
	};

	eb->buffers_ptr = to_user_pointer(&obj);

	for (unsigned int i = 0; i < count; i++) {
		obj.handle = batches[i].handle;
		gem_execbuf(fd, eb);
	}
}

static void evict_single_offset(int fd, const intel_ctx_t *ctx, int timeout)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct intel_execution_engine2 *e;
	unsigned int engines[I915_EXEC_RING_MASK + 1];
	struct batch *batches;
	unsigned int nengine;
	unsigned int count;
	uint64_t size, batch_size = BATCH_SIZE;

	nengine = 0;
	for_each_ctx_engine(fd, ctx, e) {
		engines[nengine++] = e->flags;
	}
	igt_require(nengine);

	size = gem_aperture_size(fd);
	if (size > 1ull<<32) /* Limit to 4GiB as we do not use allow-48b */
		size = 1ull << 32;
	igt_require(size < (1ull<<32) * BATCH_SIZE);

	count = size / BATCH_SIZE + 1;
	igt_debug("Using %'d batches (size: %'dMB) to fill %'llu MB aperture on "
		  "%d engines (timeout: %d)\n", count, BATCH_SIZE >> 20,
		  (long long)size >> 20, nengine, timeout);

	igt_require_memory(count, BATCH_SIZE, CHECK_RAM);
	intel_detect_and_clear_missed_interrupts(fd);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffer_count = 1;
	execbuf.rsvd1 = ctx->id;

	batches = calloc(count, sizeof(*batches));
	igt_assert(batches);
	for (unsigned int i = 0; i < count; i++)
		batches[i].handle = batch_create(fd, &batch_size);

	/* Flush all memory before we start the timer */
	single_offset_submit(fd, &execbuf, batches, count);

	igt_fork(child, nengine) {
		execbuf.flags |= engines[child];
		igt_until_timeout(timeout)
			single_offset_submit(fd, &execbuf, batches, count);
	}
	igt_waitchildren();

	for (unsigned int i = 0; i < count; i++)
		gem_close(fd, batches[i].handle);
	free(batches);

	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

struct thread {
	pthread_t thread;
	pthread_mutex_t *mutex;
	pthread_cond_t *cond;
	int *scratch;
	const intel_ctx_t *ctx;
	unsigned int engine;
	int fd, *go;
};

#define NUMOBJ 16

static void *thread(void *data)
{
	struct thread *t = data;
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	const intel_ctx_t *ctx = NULL;
	uint64_t offset_obj, offset_bb;
	uint32_t batch = MI_BATCH_BUFFER_END;
	int fd, ret, succeeded = 0;

	fd = gem_reopen_driver(t->fd);
	ctx = intel_ctx_create(fd, &t->ctx->cfg);
	offset_obj = gem_detect_safe_start_offset(fd);
	offset_bb = ALIGN(offset_obj + 4096, gem_detect_safe_alignment(fd));
	igt_debug("reopened fd: %d, ctx: %u, object offset: %llx, bb offset: %llx\n",
		  fd, ctx->id, (long long) offset_obj, (long long) offset_bb);

	pthread_mutex_lock(t->mutex);
	while (*t->go == 0)
		pthread_cond_wait(t->cond, t->mutex);
	pthread_mutex_unlock(t->mutex);

	memset(obj, 0, sizeof(obj));
	obj[0].offset = offset_obj;
	obj[0].flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_WRITE |
		       EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	obj[1].handle = gem_create(fd, 4096);
	obj[1].offset = offset_bb;
	obj[1].flags |= EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	gem_write(fd, obj[1].handle, 0, &batch, sizeof(batch));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	execbuf.flags = t->engine;
	execbuf.flags |= I915_EXEC_HANDLE_LUT;
	execbuf.flags |= I915_EXEC_NO_RELOC;
	execbuf.rsvd1 = ctx->id;

	igt_until_timeout(1) {
		unsigned int x = rand() % NUMOBJ;

		obj[0].handle = prime_fd_to_handle(fd, t->scratch[x]);

		ret = __gem_execbuf(fd, &execbuf);
		if (ret)
			igt_debug("<fd: %d, ctx: %u, x: %2u, engine: %d> "
				  "object handle: %2u (prime fd: %2d), bb handle: %2u, "
				  "offsets: %llx, %llx [ret: %d, succeeded: %d]\n",
				  fd, ctx->id, x, t->engine,
				  obj[0].handle, t->scratch[x], obj[1].handle,
				  (long long) obj[0].offset,
				  (long long) obj[1].offset, ret, succeeded);
		else
			succeeded++;

		gem_close(fd, obj[0].handle);

		if (ret)
			break;
	}

	if (!ret)
		igt_debug("<fd: %d, ctx: %u, engine: %d> succeeded: %d\n",
			  fd, ctx->id, t->engine, succeeded);
	intel_ctx_destroy(fd, ctx);
	gem_close(fd, obj[1].handle);
	close(fd);

	return (void *) from_user_pointer(ret);
}

static void evict_prime(int fd, const intel_ctx_t *ctx,
			const struct intel_execution_engine2 *engine,
			int numthreads)
{
	unsigned int engines[I915_EXEC_RING_MASK + 1], nengine;
	uint32_t handle[NUMOBJ];
	int scratch[NUMOBJ];
	struct thread *threads;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int go;
	int i;
	bool failed = false;

	igt_require(igt_allow_unlimited_files());

	nengine = 0;
	if (!engine) {
		struct intel_execution_engine2 *e;

		for_each_ctx_engine(fd, ctx, e)
			engines[nengine++] = e->flags;
	} else {
		engines[nengine++] = engine->flags;
	}
	igt_require(nengine);

	for (i = 0; i < NUMOBJ; i++) {
		handle[i] = gem_create(fd, 4096);
		scratch[i] = prime_handle_to_fd(fd, handle[i]);
	}

	threads = calloc(numthreads, sizeof(struct thread));
	igt_assert(numthreads);

	intel_detect_and_clear_missed_interrupts(fd);
	pthread_mutex_init(&mutex, 0);
	pthread_cond_init(&cond, 0);
	go = 0;

	for (i = 0; i < numthreads; i++) {
		threads[i].fd = fd;
		threads[i].ctx = ctx;
		threads[i].engine = engines[i % nengine];
		threads[i].scratch = scratch;
		threads[i].mutex = &mutex;
		threads[i].cond = &cond;
		threads[i].go = &go;

		pthread_create(&threads[i].thread, 0, thread, &threads[i]);
	}

	pthread_mutex_lock(&mutex);
	go = numthreads;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);

	for (i = 0; i < numthreads; i++) {
		void *retp;
		int ret;

		pthread_join(threads[i].thread, &retp);
		ret = (int) to_user_pointer(retp);
		if (ret)
			failed = true;
	}

	for (i = 0; i < NUMOBJ; i++) {
		gem_close(fd, handle[i]);
		close(scratch[i]);
	}

	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
	free(threads);
	igt_assert_eq(failed, false);
}

static void make_batch(int i915, uint32_t handle, uint64_t size)
{
	uint32_t *bb = gem_mmap__device_coherent(i915, handle, 0, size, PROT_WRITE);
	*bb = MI_BATCH_BUFFER_END;
	munmap(bb, size);
}

static void safe_alignment(int i915)
{
	struct drm_i915_gem_execbuffer2 execbuf = {};
	struct drm_i915_gem_exec_object2 obj[2] = {};
	uint32_t handle1, handle2, region1, region2;
	uint64_t alignment, offset1, offset2, size1 = 4096, size2 = 4096;
	const struct intel_execution_engine2 *e;
	const intel_ctx_t *ctx;

	region1 = REGION_SMEM;
	region2 = gem_has_lmem(i915) ? REGION_LMEM(0) : REGION_SMEM;
	igt_assert(__gem_create_in_memory_regions(i915, &handle1, &size1, region1) == 0);
	igt_assert(handle1);
	make_batch(i915, handle1, 4096);
	igt_assert(__gem_create_in_memory_regions(i915, &handle2, &size2, region2) == 0);
	igt_assert(handle2);
	make_batch(i915, handle2, 4096);

	offset1 = gem_detect_min_start_offset_for_region(i915, region1);
	offset2 = gem_detect_min_start_offset_for_region(i915, region2);
	alignment = gem_detect_safe_alignment(i915);
	igt_debug("safe alignment: %llx\n", (long long) alignment);
	igt_debug("safe start offset: %llx\n",
		  (long long) gem_detect_safe_start_offset(i915));
	igt_debug("minimum object1 start offset: %llx\n", (long long) offset1);
	igt_debug("minimum object2 start offset: %llx\n", (long long) offset2);

	execbuf.buffer_count = 2;
	execbuf.buffers_ptr = to_user_pointer(obj);

	obj[0].offset = offset1;
	obj[0].flags = EXEC_OBJECT_PINNED;
	obj[0].handle = handle1;
	obj[1].offset = max(ALIGN(offset1 + size1, alignment), offset2);
	obj[1].flags = EXEC_OBJECT_PINNED;
	obj[1].handle = handle2;
	igt_debug("obj[0].offset: %llx, handle: %u\n", obj[0].offset, obj[0].handle);
	igt_debug("obj[1].offset: %llx, handle: %u\n", obj[1].offset, obj[1].handle);

	gem_execbuf(i915, &execbuf);
	execbuf.flags = I915_EXEC_BATCH_FIRST;
	gem_execbuf(i915, &execbuf);

	obj[0].offset = offset2;
	obj[0].flags = EXEC_OBJECT_PINNED;
	obj[0].handle = handle2;
	obj[1].offset = max(ALIGN(offset2 + size2, alignment), offset1);
	obj[1].flags = EXEC_OBJECT_PINNED;
	obj[1].handle = handle1;
	igt_debug("obj[0].offset: %llx, handle: %u\n", obj[0].offset, obj[0].handle);
	igt_debug("obj[1].offset: %llx, handle: %u\n", obj[1].offset, obj[1].handle);

	gem_execbuf(i915, &execbuf);
	execbuf.flags = 0;
	gem_execbuf(i915, &execbuf);
	gem_sync(i915, handle1);

	/* Last check, verify safe start for each engine */
	ctx = intel_ctx_create_all_physical(i915);
	execbuf.buffer_count = 1;
	execbuf.rsvd1 = ctx->id;
	obj[0].offset = gem_detect_safe_start_offset(i915);
	for_each_ctx_engine(i915, ctx, e) {
		execbuf.flags = e->flags;
		obj[0].handle = handle1;
		gem_execbuf(i915, &execbuf);
		obj[0].handle = handle2;
		gem_execbuf(i915, &execbuf);
	}

	gem_sync(i915, handle1);
	gem_close(i915, handle1);
	gem_close(i915, handle2);
	intel_ctx_destroy(i915, ctx);
}

#define test_each_engine(T, i915, ctx, e) \
	igt_subtest_with_dynamic(T) for_each_ctx_engine(i915, ctx, e) \
		igt_dynamic_f("%s", e->name)

igt_main
{
	const struct intel_execution_engine2 *e;
	int fd = -1;
	const intel_ctx_t *ctx;

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
		igt_require_gem(fd);
		gem_require_blitter(fd);
		igt_require(gem_has_softpin(fd));
		igt_require(gem_can_store_dword(fd, 0));

		ctx = intel_ctx_create_all_physical(fd);
	}

	igt_describe("Check that invalid inputs are handled correctly.");
	igt_subtest("invalid")
		test_invalid(fd);

	igt_subtest_group {
		/* Under full-ppgtt, we have complete control of the GTT */

		igt_fixture {
			igt_require(gem_uses_full_ppgtt(fd));
		}

		igt_describe("Check full placement control under full-ppGTT.");
		igt_subtest("zero")
			test_zero(fd);

		igt_describe("Check the last 32b page is excluded.");
		igt_subtest("32b-excludes-last-page")
			test_32b_last_page(fd);

		igt_describe("Check the total occupancy by using pad-to-size to fill"
			     " the entire GTT.");
		igt_subtest("full")
			test_full(fd);

		igt_describe("Check that we can place objects at start/end of the GTT"
			     " using the allocator.");
		igt_subtest("allocator-basic")
			test_allocator_basic(fd, false);

		igt_describe("Check that if we can reserve a space for an object"
			     " starting from a given offset.");
		igt_subtest("allocator-basic-reserve")
			test_allocator_basic(fd, true);

		igt_describe("Check that we can combine manual placement with automatic"
			     " GTT placement.");
		igt_subtest("allocator-nopin")
			test_allocator_nopin(fd, false);

		igt_describe("Check that we can combine manual placement with automatic"
			     " GTT placement and reserves/unreserves space for objects.");
		igt_subtest("allocator-nopin-reserve")
			test_allocator_nopin(fd, true);

		igt_describe("Check if multiple processes can use alloctor.");
		igt_subtest("allocator-fork")
			test_allocator_fork(fd);

		igt_describe("Exercise eviction with softpinning.");
		test_each_engine("allocator-evict", fd, ctx, e)
			test_allocator_evict(fd, ctx, e->flags, 20);

		igt_describe("Use same offset for all engines and for different handles.");
		igt_subtest("evict-single-offset")
			evict_single_offset(fd, ctx, 20);

		igt_describe("Check eviction of vma on importing prime fd in reopened "
			     "drm fd in single thread");
		igt_subtest_with_dynamic("evict-prime-sanity-check") {
			for_each_ctx_engine(fd, ctx, e) {
				igt_dynamic(e->name)
					evict_prime(fd, ctx, e, 1);
			}
			igt_dynamic("all")
				evict_prime(fd, ctx, NULL, 1);
		}
		igt_describe("Check eviction of vma on importing prime fd in reopened drm fds");
		igt_subtest_with_dynamic("evict-prime") {
			for_each_ctx_engine(fd, ctx, e) {
				igt_dynamic(e->name)
					evict_prime(fd, ctx, e, 4);
			}
			igt_dynamic("all")
				evict_prime(fd, ctx, NULL, 4);
		}
	}

	igt_describe("Check start offset and alignment detection.");
	igt_subtest("safe-alignment")
		safe_alignment(fd);

	igt_describe("Check softpinning of a gem buffer object.");
	igt_subtest("softpin")
		test_softpin(fd);

	igt_describe("Check all the possible pages aligned overlaps.");
	igt_subtest("overlap")
		test_overlap(fd);

	igt_describe("Check that if the user demands the vma will be swapped.");
	igt_subtest("reverse")
		test_reverse(fd);

	igt_describe("Check that noreloc support works.");
	igt_subtest("noreloc")
		test_noreloc(fd, NOSLEEP, 0);

	igt_describe("Check noreloc support with interruptible.");
	igt_subtest("noreloc-interruptible")
		test_noreloc(fd, NOSLEEP, INTERRUPTIBLE);

	igt_describe("Check noreloc survives after suspend to RAM/resume cycle.");
	igt_subtest("noreloc-S3")
		test_noreloc(fd, SUSPEND, 0);

	igt_describe("Check noreloc survives after suspend to disk/resume cycle.");
	igt_subtest("noreloc-S4")
		test_noreloc(fd, HIBERNATE, 0);

	for (int signal = 0; signal <= 1; signal++) {
		igt_describe_f("Check eviction with active bo%s.", signal ? " with interrupts" : "");
		igt_subtest_f("evict-active%s", signal ? "-interruptible" : "")
			test_evict_active(fd, signal);

		igt_describe_f("Check eviction against snooping%s.", signal ? " with interrupts" : "");
		igt_subtest_f("evict-snoop%s", signal ? "-interruptible" : "")
			test_evict_snoop(fd, signal);
	}

	igt_describe("Check eviction of softpinned bo with hung batch.");
	igt_subtest("evict-hang")
		test_evict_hang(fd);

	igt_fixture {
		intel_ctx_destroy(fd, ctx);
		close(fd);
	}
}
