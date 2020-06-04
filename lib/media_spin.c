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
 * 	Jeff McGee <jeff.mcgee@intel.com>
 */

#include <i915_drm.h>
#include "intel_reg.h"
#include "drmtest.h"
#include "intel_batchbuffer.h"
#include "intel_chipset.h"
#include "media_spin.h"
#include "gpu_cmds.h"

static const uint32_t spin_kernel[][4] = {
	{ 0x00600001, 0x20800208, 0x008d0000, 0x00000000 }, /* mov (8)r4.0<1>:ud r0.0<8;8;1>:ud */
	{ 0x00200001, 0x20800208, 0x00450040, 0x00000000 }, /* mov (2)r4.0<1>.ud r2.0<2;2;1>:ud */
	{ 0x00000001, 0x20880608, 0x00000000, 0x00000003 }, /* mov (1)r4.8<1>:ud 0x3 */
	{ 0x00000001, 0x20a00608, 0x00000000, 0x00000000 }, /* mov (1)r5.0<1>:ud 0 */
	{ 0x00000040, 0x20a00208, 0x060000a0, 0x00000001 }, /* add (1)r5.0<1>:ud r5.0<0;1;0>:ud 1 */
	{ 0x01000010, 0x20000200, 0x02000020, 0x000000a0 }, /* cmp.e.f0.0 (1)null<1> r1<0;1;0> r5<0;1;0> */
	{ 0x00110027, 0x00000000, 0x00000000, 0xffffffe0 }, /* ~f0.0 while (1) -32 */
	{ 0x0c800031, 0x20000a00, 0x0e000080, 0x040a8000 }, /* send.dcdp1 (16)null<1> r4.0<0;1;0> 0x040a8000 */
	{ 0x00600001, 0x2e000208, 0x008d0000, 0x00000000 }, /* mov (8)r112<1>:ud r0.0<8;8;1>:ud */
	{ 0x07800031, 0x20000a40, 0x0e000e00, 0x82000010 }, /* send.ts (16)null<1> r112<0;1;0>:d 0x82000010 */
};

/*
 * This sets up the media pipeline,
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
#define THREADS 0
#define MEDIA_URB_ENTRIES 2
#define MEDIA_URB_SIZE 2
#define MEDIA_CURBE_SIZE 2

/* Offsets needed in gen_emit_media_object. In media_spin library this
 * values do not matter.
 */
#define xoffset 0
#define yoffset 0

static uint32_t
gen8_spin_curbe_buffer_data(struct intel_bb *ibb, uint32_t iters)
{
	uint32_t *curbe_buffer;
	uint32_t offset;

	intel_bb_ptr_align(ibb, 64);
	curbe_buffer = intel_bb_ptr(ibb);
	offset = intel_bb_offset(ibb);

	*curbe_buffer = iters;
	intel_bb_ptr_add(ibb, 64);

	return offset;
}

void
gen8_media_spinfunc(int i915, struct intel_buf *buf, uint32_t spins)
{
	struct intel_bb *ibb;
	uint32_t curbe_buffer, interface_descriptor;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	intel_bb_add_object(ibb, buf->handle, 0, true);

	/* setup states */
	intel_bb_ptr_set(ibb, BATCH_STATE_SPLIT);

	curbe_buffer = gen8_spin_curbe_buffer_data(ibb, spins);
	interface_descriptor = gen8_fill_interface_descriptor(ibb, buf,
					      spin_kernel, sizeof(spin_kernel));

	intel_bb_ptr_set(ibb, 0);

	/* media pipeline */
	intel_bb_out(ibb, GEN8_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA);
	gen8_emit_state_base_address(ibb);

	gen8_emit_vfe_state(ibb, THREADS, MEDIA_URB_ENTRIES,
			    MEDIA_URB_SIZE, MEDIA_CURBE_SIZE);

	gen7_emit_curbe_load(ibb, curbe_buffer);

	gen7_emit_interface_descriptor_load(ibb, interface_descriptor);

	gen_emit_media_object(ibb, xoffset, yoffset);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 32);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, false);

	intel_bb_object_offset_to_buf(ibb, buf);
	intel_bb_destroy(ibb);
}

void
gen9_media_spinfunc(int i915, struct intel_buf *buf, uint32_t spins)
{
	struct intel_bb *ibb;
	uint32_t curbe_buffer, interface_descriptor;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	intel_bb_add_object(ibb, buf->handle, 0, true);

	/* setup states */
	intel_bb_ptr_set(ibb, BATCH_STATE_SPLIT);

	curbe_buffer = gen8_spin_curbe_buffer_data(ibb, spins);
	interface_descriptor = gen8_fill_interface_descriptor(ibb, buf,
					      spin_kernel, sizeof(spin_kernel));

	intel_bb_ptr_set(ibb, 0);

	/* media pipeline */
	intel_bb_out(ibb, GEN8_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA |
		     GEN9_FORCE_MEDIA_AWAKE_ENABLE |
		     GEN9_SAMPLER_DOP_GATE_DISABLE |
		     GEN9_PIPELINE_SELECTION_MASK |
		     GEN9_SAMPLER_DOP_GATE_MASK |
		     GEN9_FORCE_MEDIA_AWAKE_MASK);
	gen9_emit_state_base_address(ibb);

	gen8_emit_vfe_state(ibb, THREADS, MEDIA_URB_ENTRIES,
			    MEDIA_URB_SIZE, MEDIA_CURBE_SIZE);

	gen7_emit_curbe_load(ibb, curbe_buffer);

	gen7_emit_interface_descriptor_load(ibb, interface_descriptor);

	gen_emit_media_object(ibb, xoffset, yoffset);

	intel_bb_out(ibb, GEN8_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA |
		     GEN9_FORCE_MEDIA_AWAKE_DISABLE |
		     GEN9_SAMPLER_DOP_GATE_ENABLE |
		     GEN9_PIPELINE_SELECTION_MASK |
		     GEN9_SAMPLER_DOP_GATE_MASK |
		     GEN9_FORCE_MEDIA_AWAKE_MASK);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);

	intel_bb_ptr_align(ibb, 32);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, false);

	intel_bb_object_offset_to_buf(ibb, buf);
	intel_bb_destroy(ibb);
}
