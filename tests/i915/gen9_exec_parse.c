/*
 * Copyright © 2018 Intel Corporation
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

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "i915/gem.h"
#include "i915/gem_submission.h"
#include "igt.h"
#include "sw_sync.h"

/* To help craft commands known to be invalid across all engines */
#define INSTR_CLIENT_SHIFT	29
#define   INSTR_INVALID_CLIENT  0x7

#define MI_LOAD_REGISTER_REG (0x2a << 23)
#define MI_STORE_REGISTER_MEM (0x24 << 23)
#define MI_ARB_ON_OFF (0x8 << 23)
#define MI_USER_INTERRUPT (0x02 << 23)
#define MI_FLUSH_DW (0x26 << 23)
#define MI_ARB_CHECK (0x05 << 23)
#define MI_REPORT_HEAD (0x07 << 23)
#define MI_SUSPEND_FLUSH (0x0b << 23)
#define MI_LOAD_SCAN_LINES_EXCL (0x13 << 23)
#define MI_UPDATE_GTT (0x23 << 23)

#define BCS_SWCTRL     0x22200
#define BCS_GPR_BASE   0x22600
#define BCS_GPR(n)     (0x22600 + (n) * 8)
#define BCS_GPR_UDW(n) (0x22600 + (n) * 8 + 4)

#define HANDLE_SIZE  4096

static int
__checked_execbuf(int i915, struct drm_i915_gem_execbuffer2 *eb)
{
	int fence;
	int err;

	igt_assert(!(eb->flags & I915_EXEC_FENCE_OUT));
	eb->flags |= I915_EXEC_FENCE_OUT;
	err = __gem_execbuf_wr(i915, eb);
	eb->flags &= ~I915_EXEC_FENCE_OUT;
	if (err)
		return err;

	fence = eb->rsvd2 >> 32;

	sync_fence_wait(fence, -1);
	err = sync_fence_status(fence);
	close(fence);
	if (err < 0)
		return err;

	return 0;
}

static int
__exec_batch_patched(int i915, int engine,
		     uint32_t cmd_bo, const uint32_t *cmds, int size,
		     uint32_t target_bo, uint64_t target_offset, uint64_t target_delta)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc[1];

	gem_write(i915, cmd_bo, 0, cmds, size);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = target_bo;
	obj[1].handle = cmd_bo;

	memset(reloc, 0, sizeof(reloc));
	reloc[0].offset = target_offset;
	reloc[0].target_handle = target_bo;
	reloc[0].delta = target_delta;
	reloc[0].read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc[0].write_domain = I915_GEM_DOMAIN_COMMAND;
	reloc[0].presumed_offset = -1;

	obj[1].relocs_ptr = to_user_pointer(reloc);
	obj[1].relocation_count = 1;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	execbuf.batch_len = size;
	execbuf.flags = engine;

	return __checked_execbuf(i915, &execbuf);
}

static void exec_batch_patched(int i915, int engine,
			       uint32_t cmd_bo, const uint32_t *cmds,
			       int size, int patch_offset,
			       long int expected_value)
{
	const uint32_t target_bo = gem_create(i915, HANDLE_SIZE);
	uint64_t actual_value = 0;
	long int ret;

	ret = __exec_batch_patched(i915, engine, cmd_bo, cmds, size, target_bo, patch_offset, 0);
	if (ret) {
		igt_assert_lt(ret, 0);
		gem_close(i915, target_bo);
		igt_assert_eq(ret, expected_value);
		return;
	}

	gem_read(i915, target_bo, 0, &actual_value, sizeof(actual_value));

	gem_close(i915, target_bo);

	igt_assert_eq(actual_value, expected_value);
}

static int __exec_batch(int i915, int engine, uint32_t cmd_bo,
			const uint32_t *cmds, int size)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[1];

	gem_write(i915, cmd_bo, 0, cmds, size);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = cmd_bo;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 1;
	execbuf.batch_len = size;
	execbuf.flags = engine;

	return  __checked_execbuf(i915, &execbuf);
}

#if 0
static void print_batch(const uint32_t *cmds, const uint32_t sz)
{
	const int commands = sz / 4;
	int i;

	igt_info("Batch size %d\n", sz);
	for (i = 0; i < commands; i++)
		igt_info("0x%08x: 0x%08x\n", i, cmds[i]);
}
#else
#define print_batch(cmds, size)
#endif

#define exec_batch(i915, engine, bo, cmds, sz, expected)	\
	print_batch(cmds, sz); \
	igt_assert_eq(__exec_batch(i915, engine, bo, cmds, sz), expected)

static void exec_split_batch(int i915, int engine, const uint32_t *cmds,
			     int size, int expected_ret)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[1];
	uint32_t cmd_bo;
	const uint32_t noop[1024] = { 0 };
	const int alloc_size = 4096 * 2;
	const int actual_start_offset = 4096-sizeof(uint32_t);

	/* Allocate and fill a 2-page batch with noops */
	cmd_bo = gem_create(i915, alloc_size);
	gem_write(i915, cmd_bo, 0, noop, sizeof(noop));
	gem_write(i915, cmd_bo, 4096, noop, sizeof(noop));

	/* Write the provided commands such that the first dword
	 * of the command buffer is the last dword of the first
	 * page (i.e. the command is split across the two pages).
	 */
	gem_write(i915, cmd_bo, actual_start_offset, cmds, size);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = cmd_bo;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 1;
	/* NB: We want batch_start_offset and batch_len to point to the block
	 * of the actual commands (i.e. at the last dword of the first page),
	 * but have to adjust both the start offset and length to meet the
	 * kernel driver's requirements on the alignment of those fields.
	 */
	execbuf.batch_start_offset = actual_start_offset & ~0x7;
	execbuf.batch_len =
		ALIGN(size + actual_start_offset - execbuf.batch_start_offset,
		      0x8);
	execbuf.flags = engine;

	igt_assert_eq(__checked_execbuf(i915, &execbuf), expected_ret);

	gem_close(i915, cmd_bo);
}

static void exec_batch_chained(int i915, int engine,
			       uint32_t cmd_bo, const uint32_t *cmds,
			       int size, int patch_offset,
			       uint64_t expected_value,
			       int expected_return)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[3];
	struct drm_i915_gem_relocation_entry reloc[1];
	struct drm_i915_gem_relocation_entry first_level_reloc;

	const uint32_t target_bo = gem_create(i915, 4096);
	const uint32_t first_level_bo = gem_create(i915, 4096);
	uint64_t actual_value = 0;
	int ret;

	const uint32_t first_level_cmds[] = {
		MI_BATCH_BUFFER_START | MI_BATCH_NON_SECURE_I965 | 1,
		0,
		0,
		MI_BATCH_BUFFER_END,
	};

	gem_write(i915, first_level_bo, 0,
		  first_level_cmds, sizeof(first_level_cmds));
	gem_write(i915, cmd_bo, 0, cmds, size);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = target_bo;
	obj[1].handle = cmd_bo;
	obj[2].handle = first_level_bo;

	memset(reloc, 0, sizeof(reloc));
	reloc[0].offset = patch_offset;
	reloc[0].delta = 0;
	reloc[0].target_handle = target_bo;
	reloc[0].read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc[0].write_domain = I915_GEM_DOMAIN_COMMAND;
	reloc[0].presumed_offset = -1;

	obj[1].relocation_count = 1;
	obj[1].relocs_ptr = to_user_pointer(&reloc);

	memset(&first_level_reloc, 0, sizeof(first_level_reloc));
	first_level_reloc.offset = 4;
	first_level_reloc.delta = 0;
	first_level_reloc.target_handle = cmd_bo;
	first_level_reloc.read_domains = I915_GEM_DOMAIN_COMMAND;
	first_level_reloc.write_domain = 0;
	obj[2].relocation_count = 1;
	obj[2].relocs_ptr = to_user_pointer(&first_level_reloc);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 3;
	execbuf.batch_len = sizeof(first_level_cmds);
	execbuf.flags = engine;

	ret = __checked_execbuf(i915, &execbuf);
	if (expected_return && ret == expected_return)
		goto out;

	gem_read(i915,target_bo, 0, &actual_value, sizeof(actual_value));

out:
	if (!expected_return)
		igt_assert_eq(expected_value, actual_value);
	else
		igt_assert_neq(expected_value, actual_value);

	gem_close(i915, first_level_bo);
	gem_close(i915, target_bo);
}

static void test_secure_batches(const int i915)
{
	int v = -1;
	drm_i915_getparam_t gp;

	gp.param = I915_PARAM_HAS_SECURE_BATCHES;
	gp.value = &v;

	igt_assert_eq(drmIoctl(i915, DRM_IOCTL_I915_GETPARAM, &gp), 0);
	igt_assert_eq(v, 0);

	igt_assert_f(gem_uses_full_ppgtt(i915),
		     "full-ppgtt required for read-only post-validated batches\n");
}

struct cmd {
	uint32_t cmd;
	int len;
	const char *name;
};

#define CMD(C, L) { .cmd = (C), .len = (L), .name = #C }
#define CMD_N(C) { .cmd = (C), .len = 1, .name = #C }

static const struct cmd allowed_cmds[] = {
	CMD_N(MI_NOOP),
	CMD_N(MI_USER_INTERRUPT),
	CMD_N(MI_WAIT_FOR_EVENT),
	CMD(MI_FLUSH_DW, 5),
	CMD_N(MI_ARB_CHECK),
	CMD_N(MI_REPORT_HEAD),
	CMD_N(MI_FLUSH),
	CMD_N(MI_ARB_ON_OFF),
	CMD_N(MI_SUSPEND_FLUSH),
	CMD(MI_LOAD_SCAN_LINES_INCL, 2),
	CMD(MI_LOAD_SCAN_LINES_EXCL, 2),
};

static uint32_t *inject_cmd(uint32_t *batch, const uint32_t cmd, int len)
{
	int i = 0;

	batch[i++] = cmd;

	while (--len)
		batch[i++] = 0;

	return &batch[i];
}

static unsigned long batch_num_cmds(const uint32_t * const batch_start,
				    const uint32_t * const batch_end)
{
	igt_assert_lte((unsigned long)batch_start, (unsigned long)batch_end);

	return batch_end - batch_start;
}

static unsigned long batch_bytes(const uint32_t * const batch_start,
				 const uint32_t * const batch_end)
{
	const unsigned long bytes = batch_num_cmds(batch_start, batch_end) * 4;

	return ALIGN(bytes, 8);
}

static void test_allowed_all(const int i915, const uint32_t handle)
{
	uint32_t batch[4096];
	uint32_t *b = &batch[0];

	for (int i = 0; i < ARRAY_SIZE(allowed_cmds); i++)
		b = inject_cmd(b, allowed_cmds[i].cmd,
			       allowed_cmds[i].len);

	b = inject_cmd(b, MI_BATCH_BUFFER_END, 1);

	exec_batch(i915, I915_EXEC_BLT, handle, batch, batch_bytes(batch, b), 0);
}

static void test_allowed_single(const int i915, const uint32_t handle)
{
	uint32_t batch[4096];

	for (int i = 0; i < ARRAY_SIZE(allowed_cmds); i++) {
		uint32_t *b = &batch[0];

		b = inject_cmd(b, allowed_cmds[i].cmd,
			       allowed_cmds[i].len);

		b = inject_cmd(b, MI_BATCH_BUFFER_END, 1);

		igt_assert_eq(__exec_batch(i915, I915_EXEC_BLT, handle,
					   batch, batch_bytes(batch, b)),
			      0);
	};
}

static void test_bb_secure(const int i915, const uint32_t handle)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[1];
	struct drm_i915_gem_relocation_entry reloc[1];

	const uint32_t batch_secure[] = {
		MI_BATCH_BUFFER_START | 1,
		12,
		0,
		MI_NOOP,
		MI_NOOP,
		MI_BATCH_BUFFER_END,
	};

	gem_write(i915, handle, 0, batch_secure, sizeof(batch_secure));

	memset(obj, 0, sizeof(obj));
	obj[0].handle = handle;

	memset(reloc, 0, sizeof(reloc));
	reloc[0].offset = 1 * sizeof(uint32_t);
	reloc[0].target_handle = handle;
	reloc[0].delta = 4 * sizeof(uint32_t);
	reloc[0].read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc[0].write_domain = 0;
	reloc[0].presumed_offset = -1;

	obj[0].relocs_ptr = to_user_pointer(reloc);
	obj[0].relocation_count = 1;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 1;
	execbuf.batch_len = sizeof(batch_secure);
	execbuf.flags = I915_EXEC_BLT;

	igt_assert_eq(__checked_execbuf(i915, &execbuf), -EACCES);
}

#define BB_START_PARAM 0
#define BB_START_OUT   1
#define BB_START_CMD   2
#define BB_START_FAR   3

static void test_bb_start(const int i915, const uint32_t handle, int test)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc[4];
	const uint32_t target_bo = gem_create(i915, 4096);
	unsigned int jump_off, footer_pos;
	uint32_t batch[1024] = {
		MI_NOOP,
		MI_NOOP,
		MI_NOOP,
		MI_NOOP,
		MI_STORE_DWORD_IMM,
		0,
		0,
		1,
		MI_STORE_DWORD_IMM,
		4,
		0,
		2,
		MI_COND_BATCH_BUFFER_END | MI_DO_COMPARE | 2,
		0,
		0,
		0,
		MI_ARB_CHECK,
	};
	const uint32_t batch_footer[] = {
		MI_BATCH_BUFFER_START | MI_BATCH_NON_SECURE_I965 | 1,
		0,
		0,
		MI_BATCH_BUFFER_END,
	};
	uint32_t *dst;

	igt_require(gem_can_store_dword(i915, I915_EXEC_BLT));

	switch (test) {
	case BB_START_PARAM:
		jump_off = 5 * sizeof(uint32_t);
		break;
	case BB_START_CMD:
	case BB_START_FAR:
		jump_off = 8 * sizeof(uint32_t);
		break;
	default:
		jump_off = 0xf00d0000;
		break;
	}

	if (test == BB_START_FAR)
		footer_pos = sizeof(batch) - sizeof(batch_footer);
	else
		footer_pos = 17 * sizeof(uint32_t);

	memcpy(batch + footer_pos / sizeof(uint32_t),
	       batch_footer, sizeof(batch_footer));
	gem_write(i915, handle, 0, batch, sizeof(batch));

	memset(obj, 0, sizeof(obj));
	obj[0].handle = target_bo;
	obj[1].handle = handle;

	memset(reloc, 0, sizeof(reloc));
	reloc[0].offset = 5 * sizeof(uint32_t);
	reloc[0].target_handle = obj[0].handle;
	reloc[0].delta = 0;
	reloc[0].read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc[0].write_domain = I915_GEM_DOMAIN_COMMAND;

	reloc[1].offset = 9 * sizeof(uint32_t);
	reloc[1].target_handle = obj[0].handle;
	reloc[1].delta = 1 * sizeof(uint32_t);
	reloc[1].read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc[1].write_domain = I915_GEM_DOMAIN_COMMAND;

	reloc[2].offset = 14 * sizeof(uint32_t);
	reloc[2].target_handle = obj[0].handle;
	reloc[2].delta = 0;
	reloc[2].read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc[2].write_domain = 0;

	reloc[3].offset = footer_pos + 1 * sizeof(uint32_t);
	reloc[3].target_handle = obj[1].handle;
	reloc[3].delta = jump_off;
	reloc[3].read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc[3].write_domain = 0;
	reloc[3].presumed_offset = -1;

	obj[1].relocs_ptr = to_user_pointer(reloc);
	obj[1].relocation_count = ARRAY_SIZE(reloc);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	execbuf.batch_len = sizeof(batch);
	execbuf.flags = I915_EXEC_BLT;

	dst = gem_mmap__wc(i915, obj[0].handle, 0, 4096, PROT_WRITE);

	igt_assert_eq(dst[0], 0);
	igt_assert_eq(dst[1], 0);

	switch (test) {
	case BB_START_PARAM:
	case BB_START_OUT:
		igt_assert_eq(__checked_execbuf(i915, &execbuf), -EINVAL);
		break;

	case BB_START_CMD:
	case BB_START_FAR:
		gem_execbuf(i915, &execbuf);

		while (READ_ONCE(dst[0]) == 0)
		       ;

		while (READ_ONCE(dst[1]) == 0)
			;

		igt_assert_eq(dst[0], 1);
		igt_assert_eq(dst[1], 2);

		dst[0] = 0;
		__sync_synchronize();
		break;
	}

	gem_munmap(dst, 4096);
	gem_close(i915, target_bo);
}

static void test_bb_large(int i915)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	static const uint32_t sizes[] = {
		(1ull << 30) - 4096,
		(1ull << 30) + 4096,
		(2ull << 30) - 4096,
		(2ull << 30) + 4096,
		(3ull << 30) - 4096,
		(3ull << 30) + 4096,
		(4ull << 30) - 4096 /* upper bound of execbuf2 uAPI */
	};
	struct drm_i915_gem_exec_object2 obj = {};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = I915_EXEC_BLT,
	};
	uint64_t required, total;
	int i;

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		if (!__intel_check_memory(2, sizes[i], CHECK_RAM,
					  &required, &total))
			break;

		igt_debug("Using object size %#x\n", sizes[i]);
		obj.handle = gem_create(i915, sizes[i]),
		gem_write(i915, obj.handle, sizes[i] - 64, &bbe, sizeof(bbe));

		execbuf.batch_start_offset = 0;
		igt_assert_eq(__checked_execbuf(i915, &execbuf), 0);

		execbuf.batch_start_offset = sizes[i] - 64;
		igt_assert_eq(__checked_execbuf(i915, &execbuf), 0);

		gem_close(i915, obj.handle);
	}

	igt_require_f(i > 0 && sizes[i - 1] > 1ull << 31,
		      "Insufficient free memory, require at least %'"PRIu64"MiB but only have %'"PRIu64"MiB available\n",
		      required >> 20, total >> 20);
}

static void test_bb_oversize(int i915)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj = {
		.handle = gem_create(i915, 8ull << 30),
		.flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.batch_start_offset = (4ull << 30) - 4096,
		.flags = I915_EXEC_BLT,
	};

	intel_require_memory(2, 8ull << 30, CHECK_RAM);
	gem_write(i915, obj.handle, (4ull << 30) - sizeof(bbe),
		  &bbe, sizeof(bbe));

	igt_assert_eq(__checked_execbuf(i915, &execbuf), 0);

	for (int i = 13; i <= 32; i++) {
		igt_debug("Checking length %#llx\n", 1ull << i);

		execbuf.batch_len = (1ull << i) - 4096;
		igt_assert_eq(__checked_execbuf(i915, &execbuf), 0);

		execbuf.batch_len = (1ull << i) + 4096; /* will wrap */
		igt_assert_eq(__checked_execbuf(i915, &execbuf), 0);
	}

	execbuf.batch_len = 0;
	igt_assert_eq(__checked_execbuf(i915, &execbuf), 0);

	gem_close(i915, obj.handle);
}

static void test_bb_chained(const int i915, const uint32_t handle)
{
	const uint32_t batch[] = {
		(0x20 << 23) | 2, /* MI_STORE_DATA_IMM */
		0,
		0,
		0xbaadf00d,
		MI_NOOP,
		MI_BATCH_BUFFER_END,
	};

	exec_batch_chained(i915, I915_EXEC_RENDER,
			   handle,
			   batch, sizeof(batch),
			   4,
			   0xbaadf00d,
			   0);

	exec_batch_chained(i915, I915_EXEC_BLT,
			   handle,
			   batch, sizeof(batch),
			   4,
			   0xbaadf00d,
			   EPERM);
}

static void test_cmd_crossing_page(const int i915, const uint32_t handle)
{
	const uint32_t lri_ok[] = {
		MI_LOAD_REGISTER_IMM,
		BCS_GPR(0),
		0xbaadf00d,
		MI_BATCH_BUFFER_END,
	};
	const uint32_t store_reg[] = {
		MI_STORE_REGISTER_MEM | (4 - 2),
		BCS_GPR(0),
		0, /* reloc */
		0, /* reloc */
		MI_NOOP,
		MI_BATCH_BUFFER_END,
	};

	exec_split_batch(i915, I915_EXEC_BLT,
			 lri_ok, sizeof(lri_ok),
			 0);

	exec_batch_patched(i915, I915_EXEC_BLT, handle,
			   store_reg, sizeof(store_reg),
			   2 * sizeof(uint32_t), /* reloc */
			   0xbaadf00d);
}

static void test_invalid_length(const int i915, const uint32_t handle)
{
	const uint32_t ok_val = 0xbaadf00d;
	const uint32_t bad_val = 0xf00dbaad;
	const uint32_t noops[8192] = { 0, };

	const uint32_t lri_ok[] = {
		MI_LOAD_REGISTER_IMM,
		BCS_GPR(0),
		ok_val,
		MI_BATCH_BUFFER_END,
	};

	const uint32_t lri_bad[] = {
		MI_LOAD_REGISTER_IMM,
		BCS_GPR(0),
		bad_val,
		MI_BATCH_BUFFER_END,
	};

	const uint32_t store_reg[] = {
		MI_STORE_REGISTER_MEM | (4 - 2),
		BCS_GPR(0),
		0, /* reloc */
		0, /* reloc */
		MI_NOOP,
		MI_BATCH_BUFFER_END,
	};

	exec_batch(i915, I915_EXEC_BLT, handle,
		   lri_ok, sizeof(lri_ok),
		   0);

	exec_batch_patched(i915, I915_EXEC_BLT, handle,
			   store_reg, sizeof(store_reg),
			   2 * sizeof(uint32_t), /* reloc */
			   ok_val);

	exec_batch(i915, I915_EXEC_BLT, handle,
		   lri_bad, 0,
		   0);

	exec_batch_patched(i915, I915_EXEC_BLT, handle,
			   store_reg, sizeof(store_reg),
			   2 * sizeof(uint32_t), /* reloc */
			   ok_val);

	exec_batch(i915, I915_EXEC_BLT, handle,
		   lri_ok, 4096,
		   0);

	igt_assert_eq(__gem_write(i915, handle, 0, noops, 4097), -EINVAL);
}

struct reg {
	uint32_t addr;
	uint32_t mask;
	bool masked_write;
	bool privileged;
};

#define REG_M(ADDR, MASK, WM, P) { (ADDR), (MASK), (WM), (P) }
#define REG(ADDR) REG_M(ADDR, 0xffffffff, false, false)
#define REG_P(ADDR) REG_M(ADDR, 0xffffffff, false, true)

static const struct reg regs[] = {
	REG_M(BCS_SWCTRL, 0x3, true, false),
	REG(BCS_GPR(0)),
	REG(BCS_GPR_UDW(0)),
	REG(BCS_GPR(1)),
	REG(BCS_GPR_UDW(1)),
	REG(BCS_GPR(2)),
	REG(BCS_GPR_UDW(2)),
	REG(BCS_GPR(3)),
	REG(BCS_GPR_UDW(3)),
	REG(BCS_GPR(4)),
	REG(BCS_GPR_UDW(4)),
	REG(BCS_GPR(5)),
	REG(BCS_GPR_UDW(5)),
	REG(BCS_GPR(6)),
	REG(BCS_GPR_UDW(6)),
	REG(BCS_GPR(7)),
	REG(BCS_GPR_UDW(7)),
	REG(BCS_GPR(8)),
	REG(BCS_GPR_UDW(8)),
	REG(BCS_GPR(9)),
	REG(BCS_GPR_UDW(9)),
	REG(BCS_GPR(10)),
	REG(BCS_GPR_UDW(10)),
	REG(BCS_GPR(11)),
	REG(BCS_GPR_UDW(11)),
	REG(BCS_GPR(12)),
	REG(BCS_GPR_UDW(12)),
	REG(BCS_GPR(13)),
	REG(BCS_GPR_UDW(13)),
	REG(BCS_GPR(14)),
	REG(BCS_GPR_UDW(14)),
	REG(BCS_GPR(15)),
	REG(BCS_GPR_UDW(15)),

	REG_P(0),
	REG_P(200000),

	REG_P(BCS_SWCTRL - 1),
	REG_P(BCS_SWCTRL - 2),
	REG_P(BCS_SWCTRL - 3),
	REG_P(BCS_SWCTRL - 4),
	REG_P(BCS_SWCTRL + 4),

	REG_P(BCS_GPR(0) - 1),
	REG_P(BCS_GPR(0) - 2),
	REG_P(BCS_GPR(0) - 3),
	REG_P(BCS_GPR(0) - 4),
	REG_P(BCS_GPR_UDW(15) + 4),
};

static void test_register(const int i915, const uint32_t handle,
			  const struct reg *r)
{
	const uint32_t lri_zero[] = {
		MI_LOAD_REGISTER_IMM,
		r->addr,
		r->masked_write ? 0xffff0000 : 0,
		MI_BATCH_BUFFER_END,
	};

	const uint32_t lri_mask[] = {
		MI_LOAD_REGISTER_IMM,
		r->addr,
		r->masked_write ? (r->mask << 16) | r->mask : r->mask,
		MI_BATCH_BUFFER_END,
	};

	const uint32_t store_reg[] = {
		MI_STORE_REGISTER_MEM | (4 - 2),
		r->addr,
		0, /* reloc */
		0, /* reloc */
		MI_NOOP,
		MI_BATCH_BUFFER_END,
	};

	exec_batch(i915, I915_EXEC_BLT, handle,
		   lri_mask, sizeof(lri_mask),
		   r->privileged ? -EACCES : 0);

	exec_batch_patched(i915, I915_EXEC_BLT, handle,
			   store_reg, sizeof(store_reg),
			   2 * sizeof(uint32_t), /* reloc */
			   r->privileged ? -EACCES : r->mask);

	exec_batch(i915, I915_EXEC_BLT, handle,
		   lri_zero, sizeof(lri_zero),
		   r->privileged ? -EACCES : 0);

	exec_batch_patched(i915, I915_EXEC_BLT, handle,
			   store_reg, sizeof(store_reg),
			   2 * sizeof(uint32_t), /* reloc */
			   r->privileged ? -EACCES : 0);
}

static void test_valid_registers(const int i915, const uint32_t handle)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(regs); i++)
		test_register(i915, handle, &regs[i]);
}

static long int read_reg(const int i915, const uint32_t handle,
			 const uint32_t addr)
{
	const uint32_t store_reg[] = {
		MI_STORE_REGISTER_MEM | (4 - 2),
		addr,
		0, /* reloc */
		0, /* reloc */
		MI_NOOP,
		MI_BATCH_BUFFER_END,
	};
	uint32_t target_bo;
	uint32_t value;
	long int ret;

	target_bo = gem_create(i915, HANDLE_SIZE);

	ret = __exec_batch_patched(i915, I915_EXEC_BLT, handle,
				   store_reg, sizeof(store_reg),
				   target_bo, 2 * sizeof(uint32_t), 0);

	if (ret) {
		igt_assert_lt(ret, 0);
		gem_close(i915, target_bo);
		return ret;
	}

	gem_read(i915, target_bo, 0, &value, sizeof(value));

	gem_close(i915, target_bo);

	return value;
}

static int write_reg(const int i915, const uint32_t handle,
		     const uint32_t addr, const uint32_t val)
{
	const uint32_t lri[] = {
		MI_LOAD_REGISTER_IMM,
		addr,
		val,
		MI_BATCH_BUFFER_END,
	};

	return __exec_batch(i915, I915_EXEC_BLT, handle,
			    lri, sizeof(lri));
}

static void test_unaligned_access(const int i915, const uint32_t handle)
{
	const uint32_t addr = BCS_GPR(4);
	const uint32_t val = 0xbaadfead;
	const uint32_t pre = 0x12345678;
	const uint32_t post = 0x87654321;

	igt_assert_eq(write_reg(i915, handle, addr - 4, pre),  0);
	igt_assert_eq(write_reg(i915, handle, addr, val),      0);
	igt_assert_eq(write_reg(i915, handle, addr + 4, post), 0);

	igt_assert_eq(read_reg(i915, handle, addr - 4), pre);
	igt_assert_eq(read_reg(i915, handle, addr),     val);
	igt_assert_eq(read_reg(i915, handle, addr + 4), post);

	for (int i = 0; i < 4; i++) {
		igt_assert_eq(write_reg(i915, handle, addr + i, val), 0);
		igt_assert_eq(read_reg(i915, handle, addr), val);

		igt_assert_eq(read_reg(i915, handle, addr + 1), val);
		igt_assert_eq(read_reg(i915, handle, addr + 2), val);
		igt_assert_eq(read_reg(i915, handle, addr + 3), val);
		igt_assert_eq(read_reg(i915, handle, addr + 4), post);
		igt_assert_eq(read_reg(i915, handle, addr - 3), pre);
		igt_assert_eq(read_reg(i915, handle, addr - 2), pre);
		igt_assert_eq(read_reg(i915, handle, addr - 1), pre);
	}
}

static void test_unaligned_jump(const int i915, const uint32_t handle)
{
	uint32_t xy[] = {
		2 << 29 | 0x53 << 22 | (10 - 2), /* XY_SRC_COPY */
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0,
	};
	uint32_t batch[] = {
		MI_BATCH_BUFFER_START | MI_BATCH_NON_SECURE_I965 | 1,
		0,
		0,
	};
	struct drm_i915_gem_relocation_entry reloc = {
		.target_handle = handle,
		.offset = 1001 * sizeof(uint32_t),
	};
	struct drm_i915_gem_exec_object2 obj = {
		.handle = handle,
		.relocs_ptr = to_user_pointer(&reloc),
		.relocation_count = 1,
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = I915_EXEC_BLT,
	};

	for (reloc.delta = 0; reloc.delta < 4000; reloc.delta += sizeof(xy))
		gem_write(i915, handle, reloc.delta, xy, sizeof(xy));

	for (reloc.delta = 0; reloc.delta < 4000; reloc.delta += 4) {
		uint64_t offset = reloc.delta + reloc.presumed_offset;

		if ((reloc.delta % sizeof(xy)) == 0)
			continue;

		memcpy(&batch[1], &offset, sizeof(offset));
		gem_write(i915, handle, 4000, batch, sizeof(batch));

		igt_assert_f(__checked_execbuf(i915, &execbuf) == -EINVAL,
			     "unaligned jump accepted to %d; batch=%08x\n",
			     reloc.delta, batch[reloc.delta / 4]);
	}
}

static void
test_reject_on_engine(int i915, uint32_t handle, unsigned int engine)
{
	const uint32_t invalid_cmd[] = {
		INSTR_INVALID_CLIENT << INSTR_CLIENT_SHIFT,
		MI_BATCH_BUFFER_END,
	};
	const uint32_t invalid_set_context[] = {
		MI_SET_CONTEXT | 32, /* invalid length */
		MI_BATCH_BUFFER_END,
	};

	exec_batch(i915, engine, handle,
		   invalid_cmd, sizeof(invalid_cmd),
		   -EINVAL);

	exec_batch(i915, engine, handle,
		   invalid_set_context, sizeof(invalid_set_context),
		   -EINVAL);
}

static void test_rejected(int i915, uint32_t handle, bool ctx_param)
{
#define engine_class(e, n) ((e)->engines[(n)].engine_class)
#define engine_instance(e, n) ((e)->engines[(n)].engine_instance)

	if (ctx_param) {
		int i;

		I915_DEFINE_CONTEXT_PARAM_ENGINES(engines , I915_EXEC_RING_MASK + 1);
		struct drm_i915_gem_context_param param = {
			.ctx_id = 0,
			.param = I915_CONTEXT_PARAM_ENGINES,
			.value = to_user_pointer(&engines),
			.size = sizeof(engines),
		};

		memset(&engines, 0, sizeof(engines));
		for (i = 0; i <= I915_EXEC_RING_MASK; i++) {
			engine_class(&engines, i) = I915_ENGINE_CLASS_COPY;
			engine_instance(&engines, i) = 0;
		}
		gem_context_set_param(i915, &param);

		for (i = 0; i <= I915_EXEC_RING_MASK; i++)
			test_reject_on_engine(i915, handle, i);

		param.size = 0;
		gem_context_set_param(i915, &param);
	} else {
		test_reject_on_engine(i915, handle, I915_EXEC_BLT);
	}
}

igt_main
{
	uint32_t handle;
	int i915;

	igt_fixture {
		i915 = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(i915);
		gem_require_blitter(i915);

		igt_require(gem_cmdparser_version(i915, I915_EXEC_BLT) >= 10);
		igt_require(intel_gen(intel_get_drm_devid(i915)) == 9);

		handle = gem_create(i915, HANDLE_SIZE);

		igt_fork_hang_detector(i915);
	}

	igt_subtest("secure-batches")
		test_secure_batches(i915);

	igt_subtest("allowed-all")
		test_allowed_all(i915, handle);

	igt_subtest("allowed-single")
		test_allowed_single(i915, handle);

	igt_subtest("bb-start-param")
		test_bb_start(i915, handle, BB_START_PARAM);

	igt_subtest("bb-start-out")
		test_bb_start(i915, handle, BB_START_OUT);

	igt_subtest("bb-secure")
		test_bb_secure(i915, handle);

	igt_subtest("bb-chained")
		test_bb_chained(i915, handle);

	igt_subtest("cmd-crossing-page")
		test_cmd_crossing_page(i915, handle);

	igt_subtest("batch-without-end") {
		const uint32_t noop[1024] = { 0 };

		exec_batch(i915, I915_EXEC_BLT, handle,
			   noop, sizeof(noop),
			   -EINVAL);
	}

	igt_subtest("batch-zero-length") {
		const uint32_t noop[] = { 0, MI_BATCH_BUFFER_END };

		exec_batch(i915, I915_EXEC_BLT, handle,
			   noop, 0,
			   -EINVAL);
	}

	igt_subtest("batch-invalid-length")
		test_invalid_length(i915, handle);

	igt_subtest("basic-rejected")
		test_rejected(i915, handle, false);

	igt_subtest("basic-rejected-ctx-param")
		test_rejected(i915, handle, true);

	igt_subtest("valid-registers")
		test_valid_registers(i915, handle);

	igt_subtest("unaligned-access")
		test_unaligned_access(i915, handle);

	igt_subtest("unaligned-jump")
		test_unaligned_jump(i915, handle);

	igt_subtest("bb-start-cmd")
		test_bb_start(i915, handle, BB_START_CMD);

	igt_subtest("bb-start-far")
		test_bb_start(i915, handle, BB_START_FAR);

	igt_subtest("bb-large")
		test_bb_large(i915);
	igt_subtest("bb-oversize")
		test_bb_oversize(i915);

	igt_fixture {
		igt_stop_hang_detector();
		gem_close(i915, handle);

		close(i915);
	}
}
