/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#include "i915/gem_create.h"
#include "igt_core.h"
#include "drmtest.h"
#include "igt_store.h"
#include "intel_chipset.h"
#include "intel_reg.h"
#include "ioctl_wrappers.h"
#include "lib/intel_allocator.h"

/**
 * SECTION:igt_store_word
 * @short_description: Library for writing a value to memory
 * @title: StoreWord
 * @include: igt.h
 *
 * A lot of igt testcases need some mechanism for writing a value to memory
 * as a test that a batch buffer has executed.
 *
 * NB: Requires master for STORE_DWORD on gen4/5.
 */
void igt_store_word(int fd, uint64_t ahnd, const intel_ctx_t *ctx,
		    const struct intel_execution_engine2 *e,
		    int fence, uint32_t target_handle,
		    uint64_t target_gpu_addr,
		    uint64_t store_offset, uint32_t store_value)
{
	const int SCRATCH = 0;
	const int BATCH = 1;
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t batch[16];
	uint64_t bb_offset, delta;
	int i;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = ARRAY_SIZE(obj);
	execbuf.flags = e->flags;
	execbuf.rsvd1 = ctx->id;
	if (fence != -1) {
		execbuf.flags |= I915_EXEC_FENCE_IN;
		execbuf.rsvd2 = fence;
	}
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	memset(obj, 0, sizeof(obj));
	obj[SCRATCH].handle = target_handle;

	obj[BATCH].handle = gem_create(fd, 4096);
	obj[BATCH].relocs_ptr = to_user_pointer(&reloc);
	obj[BATCH].relocation_count = !ahnd ? 1 : 0;
	bb_offset = get_offset(ahnd, obj[BATCH].handle, 4096, 0);
	memset(&reloc, 0, sizeof(reloc));

	i = 0;
	delta = sizeof(uint32_t) * store_offset;
	if (!ahnd) {
		reloc.target_handle = obj[SCRATCH].handle;
		reloc.presumed_offset = -1;
		reloc.offset = sizeof(uint32_t) * (i + 1);
		reloc.delta = lower_32_bits(delta);
		igt_assert_eq(upper_32_bits(delta), 0);
		reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc.write_domain = I915_GEM_DOMAIN_INSTRUCTION;
	} else {
		obj[SCRATCH].offset = target_gpu_addr;
		obj[SCRATCH].flags |= EXEC_OBJECT_PINNED | EXEC_OBJECT_WRITE;
		obj[BATCH].offset = bb_offset;
		obj[BATCH].flags |= EXEC_OBJECT_PINNED;
	}
	batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		uint64_t addr = target_gpu_addr + delta;
		batch[++i] = lower_32_bits(addr);
		batch[++i] = upper_32_bits(addr);
	} else if (gen >= 4) {
		batch[++i] = 0;
		batch[++i] = lower_32_bits(delta);
		igt_assert_eq(upper_32_bits(delta), 0);
		reloc.offset += sizeof(uint32_t);
	} else {
		batch[i]--;
		batch[++i] = lower_32_bits(delta);
		igt_assert_eq(upper_32_bits(delta), 0);
	}
	batch[++i] = store_value;
	batch[++i] = MI_BATCH_BUFFER_END;
	gem_write(fd, obj[BATCH].handle, 0, batch, sizeof(batch));
	gem_execbuf(fd, &execbuf);
	gem_close(fd, obj[BATCH].handle);
	put_offset(ahnd, obj[BATCH].handle);
}
