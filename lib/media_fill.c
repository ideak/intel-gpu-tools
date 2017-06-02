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

#include <intel_bufmgr.h>
#include <i915_drm.h>

#include "media_fill.h"
#include "gen7_media.h"
#include "gen8_media.h"
#include "intel_reg.h"
#include "drmtest.h"
#include "gpu_cmds.h"
#include <assert.h>

static const uint32_t gen7_media_kernel[][4] = {
	{ 0x00400001, 0x20200231, 0x00000020, 0x00000000 },
	{ 0x00600001, 0x20800021, 0x008d0000, 0x00000000 },
	{ 0x00200001, 0x20800021, 0x00450040, 0x00000000 },
	{ 0x00000001, 0x20880061, 0x00000000, 0x000f000f },
	{ 0x00800001, 0x20a00021, 0x00000020, 0x00000000 },
	{ 0x00800001, 0x20e00021, 0x00000020, 0x00000000 },
	{ 0x00800001, 0x21200021, 0x00000020, 0x00000000 },
	{ 0x00800001, 0x21600021, 0x00000020, 0x00000000 },
	{ 0x05800031, 0x24001ca8, 0x00000080, 0x120a8000 },
	{ 0x00600001, 0x2e000021, 0x008d0000, 0x00000000 },
	{ 0x07800031, 0x20001ca8, 0x00000e00, 0x82000010 },
};

static const uint32_t gen8_media_kernel[][4] = {
	{ 0x00400001, 0x20202288, 0x00000020, 0x00000000 },
	{ 0x00600001, 0x20800208, 0x008d0000, 0x00000000 },
	{ 0x00200001, 0x20800208, 0x00450040, 0x00000000 },
	{ 0x00000001, 0x20880608, 0x00000000, 0x000f000f },
	{ 0x00800001, 0x20a00208, 0x00000020, 0x00000000 },
	{ 0x00800001, 0x20e00208, 0x00000020, 0x00000000 },
	{ 0x00800001, 0x21200208, 0x00000020, 0x00000000 },
	{ 0x00800001, 0x21600208, 0x00000020, 0x00000000 },
	{ 0x0c800031, 0x24000a40, 0x0e000080, 0x120a8000 },
	{ 0x00600001, 0x2e000208, 0x008d0000, 0x00000000 },
	{ 0x07800031, 0x20000a40, 0x0e000e00, 0x82000010 },
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

#define BATCH_STATE_SPLIT 2048
/* VFE STATE params */
#define THREADS 1
#define MEDIA_URB_ENTRIES 2
#define MEDIA_URB_SIZE 2
#define MEDIA_CURBE_SIZE 2
#define GEN7_VFE_STATE_MEDIA_MODE 0

void
gen7_media_fillfunc(struct intel_batchbuffer *batch,
		    const struct igt_buf *dst,
		    unsigned int x, unsigned int y,
		    unsigned int width, unsigned int height,
		    uint8_t color)
{
	uint32_t curbe_buffer, interface_descriptor;
	uint32_t batch_end;

	intel_batchbuffer_flush(batch);

	/* setup states */
	batch->ptr = &batch->buffer[BATCH_STATE_SPLIT];

	curbe_buffer = gen7_fill_curbe_buffer_data(batch, color);
	interface_descriptor = gen7_fill_interface_descriptor(batch, dst,
					gen7_media_kernel,
					sizeof(gen7_media_kernel));
	igt_assert(batch->ptr < &batch->buffer[4095]);

	/* media pipeline */
	batch->ptr = batch->buffer;
	OUT_BATCH(GEN7_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA);
	gen7_emit_state_base_address(batch);

	gen7_emit_vfe_state(batch, THREADS, MEDIA_URB_ENTRIES, MEDIA_URB_SIZE,
			    MEDIA_CURBE_SIZE, GEN7_VFE_STATE_MEDIA_MODE);;

	gen7_emit_curbe_load(batch, curbe_buffer);

	gen7_emit_interface_descriptor_load(batch, interface_descriptor);

	gen7_emit_media_objects(batch, x, y, width, height);

	OUT_BATCH(MI_BATCH_BUFFER_END);

	batch_end = intel_batchbuffer_align(batch, 8);
	igt_assert(batch_end < BATCH_STATE_SPLIT);

	gen7_render_flush(batch, batch_end);
	intel_batchbuffer_reset(batch);
}

void
gen8_media_fillfunc(struct intel_batchbuffer *batch,
		    const struct igt_buf *dst,
		    unsigned int x, unsigned int y,
		    unsigned int width, unsigned int height,
		    uint8_t color)
{
	uint32_t curbe_buffer, interface_descriptor;
	uint32_t batch_end;

	intel_batchbuffer_flush(batch);

	/* setup states */
	batch->ptr = &batch->buffer[BATCH_STATE_SPLIT];

	curbe_buffer = gen7_fill_curbe_buffer_data(batch, color);
	interface_descriptor = gen8_fill_interface_descriptor(batch, dst,
					gen8_media_kernel,
					sizeof(gen8_media_kernel));
	igt_assert(batch->ptr < &batch->buffer[4095]);

	/* media pipeline */
	batch->ptr = batch->buffer;
	OUT_BATCH(GEN8_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA);
	gen8_emit_state_base_address(batch);

	gen8_emit_vfe_state(batch, THREADS, MEDIA_URB_ENTRIES, MEDIA_URB_SIZE,
			    MEDIA_CURBE_SIZE);

	gen7_emit_curbe_load(batch, curbe_buffer);

	gen7_emit_interface_descriptor_load(batch, interface_descriptor);

	gen7_emit_media_objects(batch, x, y, width, height);

	OUT_BATCH(MI_BATCH_BUFFER_END);

	batch_end = intel_batchbuffer_align(batch, 8);
	igt_assert(batch_end < BATCH_STATE_SPLIT);

	gen7_render_flush(batch, batch_end);
	intel_batchbuffer_reset(batch);
}

static void
__gen9_media_fillfunc(struct intel_batchbuffer *batch,
		      const struct igt_buf *dst,
		      unsigned int x, unsigned int y,
		      unsigned int width, unsigned int height,
		      uint8_t color, const uint32_t kernel[][4],
		      size_t kernel_size)
{
	uint32_t curbe_buffer, interface_descriptor;
	uint32_t batch_end;

	intel_batchbuffer_flush(batch);

	/* setup states */
	batch->ptr = &batch->buffer[BATCH_STATE_SPLIT];

	curbe_buffer = gen7_fill_curbe_buffer_data(batch, color);
	interface_descriptor = gen8_fill_interface_descriptor(batch, dst,
					kernel, kernel_size);
	assert(batch->ptr < &batch->buffer[4095]);

	/* media pipeline */
	batch->ptr = batch->buffer;
	OUT_BATCH(GEN8_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA |
		  GEN9_FORCE_MEDIA_AWAKE_ENABLE |
		  GEN9_SAMPLER_DOP_GATE_DISABLE |
		  GEN9_PIPELINE_SELECTION_MASK |
		  GEN9_SAMPLER_DOP_GATE_MASK |
		  GEN9_FORCE_MEDIA_AWAKE_MASK);
	gen9_emit_state_base_address(batch);

	gen8_emit_vfe_state(batch, THREADS, MEDIA_URB_ENTRIES, MEDIA_URB_SIZE,
			    MEDIA_CURBE_SIZE);

	gen7_emit_curbe_load(batch, curbe_buffer);

	gen7_emit_interface_descriptor_load(batch, interface_descriptor);

	gen7_emit_media_objects(batch, x, y, width, height);

	OUT_BATCH(GEN8_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA |
		  GEN9_FORCE_MEDIA_AWAKE_DISABLE |
		  GEN9_SAMPLER_DOP_GATE_ENABLE |
		  GEN9_PIPELINE_SELECTION_MASK |
		  GEN9_SAMPLER_DOP_GATE_MASK |
		  GEN9_FORCE_MEDIA_AWAKE_MASK);

	OUT_BATCH(MI_BATCH_BUFFER_END);

	batch_end = intel_batchbuffer_align(batch, 8);
	assert(batch_end < BATCH_STATE_SPLIT);

	gen7_render_flush(batch, batch_end);
	intel_batchbuffer_reset(batch);
}

void
gen9_media_fillfunc(struct intel_batchbuffer *batch,
		    const struct igt_buf *dst,
		    unsigned int x, unsigned int y,
		    unsigned int width, unsigned int height,
		    uint8_t color)
{

	__gen9_media_fillfunc(batch, dst, x, y, width, height, color,
			      gen8_media_kernel, sizeof(gen8_media_kernel));

}
