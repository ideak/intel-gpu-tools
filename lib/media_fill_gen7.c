#include <intel_bufmgr.h>
#include <i915_drm.h>

#include "media_fill.h"
#include "gen7_media.h"
#include "intel_reg.h"
#include "drmtest.h"
#include "gpu_fill.h"
#include <assert.h>

static const uint32_t media_kernel[][4] = {
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
gen7_media_fillfunc(struct intel_batchbuffer *batch,
		struct igt_buf *dst,
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
					media_kernel, sizeof(media_kernel));
	igt_assert(batch->ptr < &batch->buffer[4095]);

	/* media pipeline */
	batch->ptr = batch->buffer;
	OUT_BATCH(GEN7_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA);
	gen7_emit_state_base_address(batch);

	gen7_emit_vfe_state(batch);

	gen7_emit_curbe_load(batch, curbe_buffer);

	gen7_emit_interface_descriptor_load(batch, interface_descriptor);

	gen7_emit_media_objects(batch, x, y, width, height);

	OUT_BATCH(MI_BATCH_BUFFER_END);

	batch_end = intel_batchbuffer_align(batch, 8);
	igt_assert(batch_end < BATCH_STATE_SPLIT);

	gen7_render_flush(batch, batch_end);
	intel_batchbuffer_reset(batch);
}
