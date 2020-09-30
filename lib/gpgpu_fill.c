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
 *  Zhenyu Wang <zhenyuw@linux.intel.com>
 *  Dominik Zeromski <dominik.zeromski@intel.com>
 */

#include <i915_drm.h>

#include "intel_reg.h"
#include "drmtest.h"

#include "gpgpu_fill.h"
#include "gpu_cmds.h"

/* lib/i915/shaders/gpgpu/gpgpu_fill.gxa */
static const uint32_t gen7_gpgpu_kernel[][4] = {
	{ 0x00400001, 0x20200231, 0x00000020, 0x00000000 },
	{ 0x00000041, 0x20400c21, 0x00000004, 0x00000010 },
	{ 0x00000001, 0x20440021, 0x00000018, 0x00000000 },
	{ 0x00600001, 0x20800021, 0x008d0000, 0x00000000 },
	{ 0x00200001, 0x20800021, 0x00450040, 0x00000000 },
	{ 0x00000001, 0x20880061, 0x00000000, 0x0000000f },
	{ 0x00800001, 0x20a00021, 0x00000020, 0x00000000 },
	{ 0x05800031, 0x24001ca8, 0x00000080, 0x060a8000 },
	{ 0x00600001, 0x2e000021, 0x008d0000, 0x00000000 },
	{ 0x07800031, 0x20001ca8, 0x00000e00, 0x82000010 },
};

static const uint32_t gen8_gpgpu_kernel[][4] = {
	{ 0x00400001, 0x20202288, 0x00000020, 0x00000000 },
	{ 0x00000041, 0x20400208, 0x06000004, 0x00000010 },
	{ 0x00000001, 0x20440208, 0x00000018, 0x00000000 },
	{ 0x00600001, 0x20800208, 0x008d0000, 0x00000000 },
	{ 0x00200001, 0x20800208, 0x00450040, 0x00000000 },
	{ 0x00000001, 0x20880608, 0x00000000, 0x0000000f },
	{ 0x00800001, 0x20a00208, 0x00000020, 0x00000000 },
	{ 0x0c800031, 0x24000a40, 0x0e000080, 0x060a8000 },
	{ 0x00600001, 0x2e000208, 0x008d0000, 0x00000000 },
	{ 0x07800031, 0x20000a40, 0x0e000e00, 0x82000010 },
};

static const uint32_t gen9_gpgpu_kernel[][4] = {
	{ 0x00400001, 0x20202288, 0x00000020, 0x00000000 },
	{ 0x00000041, 0x20400208, 0x06000004, 0x00000010 },
	{ 0x00000001, 0x20440208, 0x00000018, 0x00000000 },
	{ 0x00600001, 0x20800208, 0x008d0000, 0x00000000 },
	{ 0x00200001, 0x20800208, 0x00450040, 0x00000000 },
	{ 0x00000001, 0x20880608, 0x00000000, 0x0000000f },
	{ 0x00800001, 0x20a00208, 0x00000020, 0x00000000 },
	{ 0x0c800031, 0x24000a40, 0x06000080, 0x060a8000 },
	{ 0x00600001, 0x2e000208, 0x008d0000, 0x00000000 },
	{ 0x07800031, 0x20000a40, 0x06000e00, 0x82000010 },
};

static const uint32_t gen11_gpgpu_kernel[][4] = {
	{ 0x00400001, 0x20202288, 0x00000020, 0x00000000 },
	{ 0x00000009, 0x20400208, 0x06000004, 0x00000004 },
	{ 0x00000001, 0x20440208, 0x00000018, 0x00000000 },
	{ 0x00600001, 0x20800208, 0x008d0000, 0x00000000 },
	{ 0x00200001, 0x20800208, 0x00450040, 0x00000000 },
	{ 0x00000001, 0x20880608, 0x00000000, 0x0000000f },
	{ 0x00800001, 0x20a00208, 0x00000020, 0x00000000 },
	{ 0x0c800031, 0x24000a40, 0x06000080, 0x040a8000 },
	{ 0x00600001, 0x2e000208, 0x008d0000, 0x00000000 },
	{ 0x07800031, 0x20000a40, 0x06000e00, 0x82000010 },
};

static const uint32_t gen12_gpgpu_kernel[][4] = {
	{ 0x00020061, 0x01050000, 0x00000104, 0x00000000 },
	{ 0x00000069, 0x02058220, 0x02000024, 0x00000004 },
	{ 0x00000061, 0x02250220, 0x000000c4, 0x00000000 },
	{ 0x00030061, 0x04050220, 0x00460005, 0x00000000 },
	{ 0x00010261, 0x04050220, 0x00220205, 0x00000000 },
	{ 0x00000061, 0x04454220, 0x00000000, 0x0000000f },
	{ 0x00040661, 0x05050220, 0x00000104, 0x00000000 },
	{ 0x00049031, 0x00000000, 0xc0000414, 0x02a00000 },
	{ 0x00030061, 0x70050220, 0x00460005, 0x00000000 },
	{ 0x00040131, 0x00000004, 0x7020700c, 0x10000000 },
};

/*
 * This sets up the gpgpu pipeline,
 *
 * +---------------+ <---- 4096
 * |       ^       |
 * |       |       |
 * |    various    |
 * |      state    |
 * |       |       |
 * |_______|_______| <---- 2048 + ?
 * |       ^       |
 * |       |       |
 * |   batch       |
 * |    commands   |
 * |       |       |
 * |       |       |
 * +---------------+ <---- 0 + ?
 *
 */

#define PAGE_SIZE 4096
#define BATCH_STATE_SPLIT 2048
/* VFE STATE params */
#define THREADS 1
#define GEN7_GPGPU_URB_ENTRIES 0
#define GEN8_GPGPU_URB_ENTRIES 1
#define GPGPU_URB_SIZE 0
#define GPGPU_CURBE_SIZE 1
#define GEN7_VFE_STATE_GPGPU_MODE 1

void
gen7_gpgpu_fillfunc(int i915,
		    struct intel_buf *buf,
		    unsigned x, unsigned y,
		    unsigned width, unsigned height,
		    uint8_t color)
{
	struct intel_bb *ibb;
	uint32_t curbe_buffer, interface_descriptor;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	intel_bb_add_intel_buf(ibb, buf, true);

	intel_bb_ptr_set(ibb, BATCH_STATE_SPLIT);

	/* Fill curbe buffer data */
	curbe_buffer = gen7_fill_curbe_buffer_data(ibb, color);

	/*
	 * const buffer needs to fill for every thread, but as we have just 1
	 * thread per every group, so need only one curbe data.
	 * For each thread, just use thread group ID for buffer offset.
	 */
	interface_descriptor =
			gen7_fill_interface_descriptor(ibb, buf,
						       gen7_gpgpu_kernel,
						       sizeof(gen7_gpgpu_kernel));

	intel_bb_ptr_set(ibb, 0);

	/* GPGPU pipeline */
	intel_bb_out(ibb, GEN7_PIPELINE_SELECT | PIPELINE_SELECT_GPGPU);

	gen7_emit_state_base_address(ibb);
	gen7_emit_vfe_state(ibb, THREADS, GEN7_GPGPU_URB_ENTRIES,
			       GPGPU_URB_SIZE, GPGPU_CURBE_SIZE,
			       GEN7_VFE_STATE_GPGPU_MODE);
	gen7_emit_curbe_load(ibb, curbe_buffer);
	gen7_emit_interface_descriptor_load(ibb, interface_descriptor);
	gen7_emit_gpgpu_walk(ibb, x, y, width, height);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 32);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, true);

	intel_bb_destroy(ibb);
}

void
gen8_gpgpu_fillfunc(int i915,
		    struct intel_buf *buf,
		    unsigned x, unsigned y,
		    unsigned width, unsigned height,
		    uint8_t color)
{
	struct intel_bb *ibb;
	uint32_t curbe_buffer, interface_descriptor;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	intel_bb_add_intel_buf(ibb, buf, true);

	intel_bb_ptr_set(ibb, BATCH_STATE_SPLIT);

	/*
	 * const buffer needs to fill for every thread, but as we have just 1
	 * thread per every group, so need only one curbe data.
	 * For each thread, just use thread group ID for buffer offset.
	 */
	curbe_buffer = gen7_fill_curbe_buffer_data(ibb, color);

	interface_descriptor = gen8_fill_interface_descriptor(ibb, buf,
				gen8_gpgpu_kernel, sizeof(gen8_gpgpu_kernel));

	intel_bb_ptr_set(ibb, 0);

	/* GPGPU pipeline */
	intel_bb_out(ibb, GEN7_PIPELINE_SELECT | PIPELINE_SELECT_GPGPU);

	gen8_emit_state_base_address(ibb);
	gen8_emit_vfe_state(ibb, THREADS, GEN8_GPGPU_URB_ENTRIES,
			    GPGPU_URB_SIZE, GPGPU_CURBE_SIZE);

	gen7_emit_curbe_load(ibb, curbe_buffer);
	gen7_emit_interface_descriptor_load(ibb, interface_descriptor);

	gen8_emit_gpgpu_walk(ibb, x, y, width, height);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 32);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, true);

	intel_bb_destroy(ibb);
}

static void
__gen9_gpgpu_fillfunc(int i915,
		      struct intel_buf *buf,
		      unsigned x, unsigned y,
		      unsigned width, unsigned height,
		      uint8_t color,
		      const uint32_t kernel[][4], size_t kernel_size)
{
	struct intel_bb *ibb;
	uint32_t curbe_buffer, interface_descriptor;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	intel_bb_add_intel_buf(ibb, buf, true);

	intel_bb_ptr_set(ibb, BATCH_STATE_SPLIT);

	/*
	 * const buffer needs to fill for every thread, but as we have just 1
	 * thread per every group, so need only one curbe data.
	 * For each thread, just use thread group ID for buffer offset.
	 */
	/* Fill curbe buffer data */
	curbe_buffer = gen7_fill_curbe_buffer_data(ibb, color);

	interface_descriptor = gen8_fill_interface_descriptor(ibb, buf,
							      kernel,
							      kernel_size);

	intel_bb_ptr_set(ibb, 0);

	/* GPGPU pipeline */
	intel_bb_out(ibb, GEN7_PIPELINE_SELECT | GEN9_PIPELINE_SELECTION_MASK |
		     PIPELINE_SELECT_GPGPU);

	gen9_emit_state_base_address(ibb);

	gen8_emit_vfe_state(ibb, THREADS, GEN8_GPGPU_URB_ENTRIES,
			    GPGPU_URB_SIZE, GPGPU_CURBE_SIZE);

	gen7_emit_curbe_load(ibb, curbe_buffer);
	gen7_emit_interface_descriptor_load(ibb, interface_descriptor);

	gen8_emit_gpgpu_walk(ibb, x, y, width, height);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 32);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, true);

	intel_bb_destroy(ibb);
}

void gen9_gpgpu_fillfunc(int i915,
			 struct intel_buf *buf,
			 unsigned x, unsigned y,
			 unsigned width, unsigned height,
			 uint8_t color)
{
	__gen9_gpgpu_fillfunc(i915, buf, x, y, width, height, color,
			      gen9_gpgpu_kernel,
			      sizeof(gen9_gpgpu_kernel));
}

void gen11_gpgpu_fillfunc(int i915,
			  struct intel_buf *buf,
			  unsigned x, unsigned y,
			  unsigned width, unsigned height,
			  uint8_t color)
{
	__gen9_gpgpu_fillfunc(i915, buf, x, y, width, height, color,
			      gen11_gpgpu_kernel,
			      sizeof(gen11_gpgpu_kernel));
}

void gen12_gpgpu_fillfunc(int i915,
			  struct intel_buf *buf,
			  unsigned x, unsigned y,
			  unsigned width, unsigned height,
			  uint8_t color)
{
	__gen9_gpgpu_fillfunc(i915, buf, x, y, width, height, color,
			      gen12_gpgpu_kernel,
			      sizeof(gen12_gpgpu_kernel));
}
