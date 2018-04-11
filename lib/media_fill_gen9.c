#include <intel_bufmgr.h>
#include <i915_drm.h>

#include "media_fill.h"
#include "gen8_media.h"
#include "intel_reg.h"
#include "gpu_fill.h"
#include <assert.h>

static const uint32_t media_kernel[][4] = {
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

void
gen9_media_fillfunc(struct intel_batchbuffer *batch,
		struct igt_buf *dst,
		unsigned x, unsigned y,
		unsigned width, unsigned height,
		uint8_t color)
{
	uint32_t curbe_buffer, interface_descriptor;
	uint32_t batch_end;

	intel_batchbuffer_flush(batch);

	/* setup states */
	batch->ptr = &batch->buffer[BATCH_STATE_SPLIT];

	curbe_buffer = gen7_fill_curbe_buffer_data(batch, color);
	interface_descriptor = gen8_fill_interface_descriptor(batch, dst, media_kernel, sizeof(media_kernel));
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

	gen8_emit_vfe_state(batch);

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

	batch_end = batch_align(batch, 8);
	assert(batch_end < BATCH_STATE_SPLIT);

	gen7_render_flush(batch, batch_end);
	intel_batchbuffer_reset(batch);
}
