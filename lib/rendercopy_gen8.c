#include <assert.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <drm.h>
#include <i915_drm.h>

#include "drmtest.h"
#include "intel_bufops.h"
#include "intel_batchbuffer.h"
#include "intel_chipset.h"
#include "intel_io.h"
#include "rendercopy.h"
#include "gen8_render.h"
#include "intel_reg.h"
#include "igt_aux.h"

#define VERTEX_SIZE (3*4)

#if DEBUG_RENDERCPY
static void dump_batch(struct intel_bb *ibb)
{
	intel_bb_dump(ibb, "/tmp/gen8-batchbuffers.dump");
}
#else
#define dump_batch(x) do { } while(0)
#endif

static struct {
	uint32_t cc_state;
	uint32_t blend_state;
} cc;

static struct {
	uint32_t cc_state;
	uint32_t sf_clip_state;
} viewport;

/* see lib/i915/shaders/ps/blit.g7a */
static const uint32_t ps_kernel[][4] = {
#if 1
   { 0x0080005a, 0x2f403ae8, 0x3a0000c0, 0x008d0040 },
   { 0x0080005a, 0x2f803ae8, 0x3a0000d0, 0x008d0040 },
   { 0x02800031, 0x2e203a48, 0x0e8d0f40, 0x08840001 },
   { 0x05800031, 0x20003a40, 0x0e8d0e20, 0x90031000 },
#else
   /* Write all -1 */
   { 0x00600001, 0x2e000608, 0x00000000, 0x3f800000 },
   { 0x00600001, 0x2e200608, 0x00000000, 0x3f800000 },
   { 0x00600001, 0x2e400608, 0x00000000, 0x3f800000 },
   { 0x00600001, 0x2e600608, 0x00000000, 0x3f800000 },
   { 0x00600001, 0x2e800608, 0x00000000, 0x3f800000 },
   { 0x00600001, 0x2ea00608, 0x00000000, 0x3f800000 },
   { 0x00600001, 0x2ec00608, 0x00000000, 0x3f800000 },
   { 0x00600001, 0x2ee00608, 0x00000000, 0x3f800000 },
   { 0x05800031, 0x200022e0, 0x0e000e00, 0x90031000 },
#endif
};

/* Mostly copy+paste from gen6, except height, width, pitch moved */
static uint32_t
gen8_bind_buf(struct intel_bb *ibb,
	      const struct intel_buf *buf, int is_dst)
{
	struct gen8_surface_state *ss;
	uint32_t write_domain, read_domain;
	uint64_t address;

	igt_assert_lte(buf->surface[0].stride, 256*1024);
	igt_assert_lte(intel_buf_width(buf), 16384);
	igt_assert_lte(intel_buf_height(buf), 16384);

	if (is_dst) {
		write_domain = read_domain = I915_GEM_DOMAIN_RENDER;
	} else {
		write_domain = 0;
		read_domain = I915_GEM_DOMAIN_SAMPLER;
	}

	ss = intel_bb_ptr_align(ibb, 64);

	ss->ss0.surface_type = SURFACE_2D;
	switch (buf->bpp) {
		case 8: ss->ss0.surface_format = SURFACEFORMAT_R8_UNORM; break;
		case 16: ss->ss0.surface_format = SURFACEFORMAT_R8G8_UNORM; break;
		case 32: ss->ss0.surface_format = SURFACEFORMAT_B8G8R8A8_UNORM; break;
		case 64: ss->ss0.surface_format = SURFACEFORMAT_R16G16B16A16_FLOAT; break;
		default: igt_assert(0);
	}
	ss->ss0.render_cache_read_write = 1;
	ss->ss0.vertical_alignment = 1; /* align 4 */
	ss->ss0.horizontal_alignment = 1; /* align 4 */
	if (buf->tiling == I915_TILING_X)
		ss->ss0.tiled_mode = 2;
	else if (buf->tiling == I915_TILING_Y)
		ss->ss0.tiled_mode = 3;

	if (IS_CHERRYVIEW(ibb->devid))
		ss->ss1.memory_object_control = CHV_MOCS_WB | CHV_MOCS_L3;
	else
		ss->ss1.memory_object_control = BDW_MOCS_PTE |
			BDW_MOCS_TC_L3_PTE | BDW_MOCS_AGE(0);

	address = intel_bb_offset_reloc(ibb, buf->handle,
					read_domain, write_domain,
					intel_bb_offset(ibb) + 4 * 8,
					buf->addr.offset);
	ss->ss8.base_addr = address;
	ss->ss9.base_addr_hi = address >> 32;

	ss->ss2.height = intel_buf_height(buf) - 1;
	ss->ss2.width  = intel_buf_width(buf) - 1;
	ss->ss3.pitch  = buf->surface[0].stride - 1;

	ss->ss7.shader_chanel_select_r = 4;
	ss->ss7.shader_chanel_select_g = 5;
	ss->ss7.shader_chanel_select_b = 6;
	ss->ss7.shader_chanel_select_a = 7;

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*ss));
}

static uint32_t
gen8_bind_surfaces(struct intel_bb *ibb,
		   const struct intel_buf *src,
		   const struct intel_buf *dst)
{
	uint32_t *binding_table, binding_table_offset;

	binding_table = intel_bb_ptr_align(ibb, 32);
	binding_table_offset = intel_bb_ptr_add_return_prev_offset(ibb, 8);

	binding_table[0] = gen8_bind_buf(ibb, dst, 1);
	binding_table[1] = gen8_bind_buf(ibb, src, 0);

	return binding_table_offset;
}

/* Mostly copy+paste from gen6, except wrap modes moved */
static uint32_t
gen8_create_sampler(struct intel_bb *ibb)
{
	struct gen8_sampler_state *ss;

	ss = intel_bb_ptr_align(ibb, 64);

	ss->ss0.min_filter = GEN4_MAPFILTER_NEAREST;
	ss->ss0.mag_filter = GEN4_MAPFILTER_NEAREST;
	ss->ss3.r_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;
	ss->ss3.s_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;
	ss->ss3.t_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;

	/* I've experimented with non-normalized coordinates and using the LD
	 * sampler fetch, but couldn't make it work. */
	ss->ss3.non_normalized_coord = 0;

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*ss));
}

static uint32_t
gen8_fill_ps(struct intel_bb *ibb,
	     const uint32_t kernel[][4],
	     size_t size)
{
	return intel_bb_copy_data(ibb, kernel, size, 64);
}

/*
 * gen7_fill_vertex_buffer_data populate vertex buffer with data.
 *
 * The vertex buffer consists of 3 vertices to construct a RECTLIST. The 4th
 * vertex is implied (automatically derived by the HW). Each element has the
 * destination offset, and the normalized texture offset (src). The rectangle
 * itself will span the entire subsurface to be copied.
 *
 * see gen6_emit_vertex_elements
 */
static uint32_t
gen7_fill_vertex_buffer_data(struct intel_bb *ibb,
			     const struct intel_buf *src,
			     uint32_t src_x, uint32_t src_y,
			     uint32_t dst_x, uint32_t dst_y,
			     uint32_t width, uint32_t height)
{
	uint32_t offset;

	intel_bb_ptr_align(ibb, 8);
	offset = intel_bb_offset(ibb);

	emit_vertex_2s(ibb, dst_x + width, dst_y + height);
	emit_vertex_normalized(ibb, src_x + width, intel_buf_width(src));
	emit_vertex_normalized(ibb, src_y + height, intel_buf_height(src));

	emit_vertex_2s(ibb, dst_x, dst_y + height);
	emit_vertex_normalized(ibb, src_x, intel_buf_width(src));
	emit_vertex_normalized(ibb, src_y + height, intel_buf_height(src));

	emit_vertex_2s(ibb, dst_x, dst_y);
	emit_vertex_normalized(ibb, src_x, intel_buf_width(src));
	emit_vertex_normalized(ibb, src_y, intel_buf_height(src));

	return offset;
}

/*
 * gen6_emit_vertex_elements - The vertex elements describe the contents of the
 * vertex buffer. We pack the vertex buffer in a semi weird way, conforming to
 * what gen6_rendercopy did. The most straightforward would be to store
 * everything as floats.
 *
 * see gen7_fill_vertex_buffer_data() for where the corresponding elements are
 * packed.
 */
static void
gen6_emit_vertex_elements(struct intel_bb *ibb) {
	/*
	 * The VUE layout
	 *    dword 0-3: pad (0, 0, 0. 0)
	 *    dword 4-7: position (x, y, 0, 1.0),
	 *    dword 8-11: texture coordinate 0 (u0, v0, 0, 1.0)
	 */
	intel_bb_out(ibb, GEN4_3DSTATE_VERTEX_ELEMENTS | (3 * 2 + 1 - 2));

	/* Element state 0. These are 4 dwords of 0 required for the VUE format.
	 * We don't really know or care what they do.
	 */
	intel_bb_out(ibb, 0 << GEN6_VE0_VERTEX_BUFFER_INDEX_SHIFT | GEN6_VE0_VALID |
		     SURFACEFORMAT_R32G32B32A32_FLOAT << VE0_FORMAT_SHIFT |
		     0 << VE0_OFFSET_SHIFT); /* we specify 0, but it's really does not exist */
	intel_bb_out(ibb, GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_0_SHIFT |
		     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_1_SHIFT |
		     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT |
		     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_3_SHIFT);

	/* Element state 1 - Our "destination" vertices. These are passed down
	 * through the pipeline, and eventually make it to the pixel shader as
	 * the offsets in the destination surface. It's packed as the 16
	 * signed/scaled because of gen6 rendercopy. I see no particular reason
	 * for doing this though.
	 */
	intel_bb_out(ibb, 0 << GEN6_VE0_VERTEX_BUFFER_INDEX_SHIFT | GEN6_VE0_VALID |
		     SURFACEFORMAT_R16G16_SSCALED << VE0_FORMAT_SHIFT |
		     0 << VE0_OFFSET_SHIFT); /* offsets vb in bytes */
	intel_bb_out(ibb, GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT |
		     GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT |
		     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT |
		     GEN4_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_3_SHIFT);

	/* Element state 2. Last but not least we store the U,V components as
	 * normalized floats. These will be used in the pixel shader to sample
	 * from the source buffer.
	 */
	intel_bb_out(ibb, 0 << GEN6_VE0_VERTEX_BUFFER_INDEX_SHIFT | GEN6_VE0_VALID |
		     SURFACEFORMAT_R32G32_FLOAT << VE0_FORMAT_SHIFT |
		     4 << VE0_OFFSET_SHIFT);	/* offset vb in bytes */
	intel_bb_out(ibb, GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT |
		     GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT |
		     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT |
		     GEN4_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_3_SHIFT);
}

/*
 * gen8_emit_vertex_buffer emit the vertex buffers command
 *
 * @batch
 * @offset - bytw offset within the @batch where the vertex buffer starts.
 */
static void gen8_emit_vertex_buffer(struct intel_bb *ibb, uint32_t offset)
{
	intel_bb_out(ibb, GEN4_3DSTATE_VERTEX_BUFFERS | (1 + (4 * 1) - 2));
	intel_bb_out(ibb, 0 << GEN6_VB0_BUFFER_INDEX_SHIFT | /* VB 0th index */
		     GEN8_VB0_BUFFER_ADDR_MOD_EN | /* Address Modify Enable */
		     VERTEX_SIZE << VB0_BUFFER_PITCH_SHIFT);
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_VERTEX, 0,
			    offset, ibb->batch_offset);
	intel_bb_out(ibb, 3 * VERTEX_SIZE);
}

static uint32_t
gen6_create_cc_state(struct intel_bb *ibb)
{
	struct gen6_color_calc_state *cc_state;

	cc_state = intel_bb_ptr_align(ibb, 64);

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*cc_state));
}

static uint32_t
gen8_create_blend_state(struct intel_bb *ibb)
{
	struct gen8_blend_state *blend;
	int i;

	blend = intel_bb_ptr_align(ibb, 64);

	for (i = 0; i < 16; i++) {
		blend->bs[i].dest_blend_factor = GEN6_BLENDFACTOR_ZERO;
		blend->bs[i].source_blend_factor = GEN6_BLENDFACTOR_ONE;
		blend->bs[i].color_blend_func = GEN6_BLENDFUNCTION_ADD;
		blend->bs[i].pre_blend_color_clamp = 1;
		blend->bs[i].color_buffer_blend = 0;
	}

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*blend));
}

static uint32_t
gen6_create_cc_viewport(struct intel_bb *ibb)
{
	struct gen4_cc_viewport *vp;

	vp = intel_bb_ptr_align(ibb, 32);

	/* XXX I don't understand this */
	vp->min_depth = -1.e35;
	vp->max_depth = 1.e35;

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*vp));
}

static uint32_t
gen7_create_sf_clip_viewport(struct intel_bb *ibb)
{
	/* XXX these are likely not needed */
	struct gen7_sf_clip_viewport *scv_state;

	scv_state = intel_bb_ptr_align(ibb, 64);

	scv_state->guardband.xmin = 0;
	scv_state->guardband.xmax = 1.0f;
	scv_state->guardband.ymin = 0;
	scv_state->guardband.ymax = 1.0f;

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*scv_state));
}

static uint32_t
gen6_create_scissor_rect(struct intel_bb *ibb)
{
	struct gen6_scissor_rect *scissor;

	scissor = intel_bb_ptr_align(ibb, 64);

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*scissor));
}

static void
gen8_emit_sip(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN4_STATE_SIP | (3 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static void
gen7_emit_push_constants(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_VS);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_HS);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_DS);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_GS);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_PS);
	intel_bb_out(ibb, 0);
}

static void
gen8_emit_state_base_address(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN4_STATE_BASE_ADDRESS | (16 - 2));

	/* general */
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);
	intel_bb_out(ibb, 0);

	/* stateless data port */
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);

	/* surface */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_SAMPLER, 0,
			    BASE_ADDRESS_MODIFY, ibb->batch_offset);

	/* dynamic */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_RENDER | I915_GEM_DOMAIN_INSTRUCTION, 0,
			    BASE_ADDRESS_MODIFY, ibb->batch_offset);

	/* indirect */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	/* instruction */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_INSTRUCTION, 0,
			    BASE_ADDRESS_MODIFY, ibb->batch_offset);

	/* general state buffer size */
	intel_bb_out(ibb, 0xfffff000 | 1);
	/* dynamic state buffer size */
	intel_bb_out(ibb, 1 << 12 | 1);
	/* indirect object buffer size */
	intel_bb_out(ibb, 0xfffff000 | 1);
	/* intruction buffer size */
	intel_bb_out(ibb, 1 << 12 | 1);
}

static void
gen7_emit_urb(struct intel_bb *ibb) {
	/* XXX: Min valid values from mesa */
	const int vs_entries = 64;
	const int vs_size = 2;
	const int vs_start = 2;

	intel_bb_out(ibb, GEN7_3DSTATE_URB_VS);
	intel_bb_out(ibb, vs_entries | ((vs_size - 1) << 16) | (vs_start << 25));
	intel_bb_out(ibb, GEN7_3DSTATE_URB_GS);
	intel_bb_out(ibb, vs_start << 25);
	intel_bb_out(ibb, GEN7_3DSTATE_URB_HS);
	intel_bb_out(ibb, vs_start << 25);
	intel_bb_out(ibb, GEN7_3DSTATE_URB_DS);
	intel_bb_out(ibb, vs_start << 25);
}

static void
gen8_emit_cc(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN7_3DSTATE_BLEND_STATE_POINTERS);
	intel_bb_out(ibb, cc.blend_state | 1);

	intel_bb_out(ibb, GEN6_3DSTATE_CC_STATE_POINTERS);
	intel_bb_out(ibb, cc.cc_state | 1);
}

static void
gen8_emit_multisample(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN8_3DSTATE_MULTISAMPLE);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN6_3DSTATE_SAMPLE_MASK);
	intel_bb_out(ibb, 1);
}

static void
gen8_emit_vs(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN7_3DSTATE_BINDING_TABLE_POINTERS_VS);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_SAMPLER_STATE_POINTERS_VS);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN6_3DSTATE_CONSTANT_VS | (11 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN6_3DSTATE_VS | (9-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static void
gen8_emit_hs(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN7_3DSTATE_CONSTANT_HS | (11 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_HS | (9-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_BINDING_TABLE_POINTERS_HS);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN8_3DSTATE_SAMPLER_STATE_POINTERS_HS);
	intel_bb_out(ibb, 0);
}

static void
gen8_emit_gs(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN6_3DSTATE_CONSTANT_GS | (11 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN6_3DSTATE_GS | (10-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_BINDING_TABLE_POINTERS_GS);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_SAMPLER_STATE_POINTERS_GS);
	intel_bb_out(ibb, 0);
}

static void
gen8_emit_ds(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN7_3DSTATE_CONSTANT_DS | (11 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_DS | (9-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_BINDING_TABLE_POINTERS_DS);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN8_3DSTATE_SAMPLER_STATE_POINTERS_DS);
	intel_bb_out(ibb, 0);
}

static void
gen8_emit_wm_hz_op(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN8_3DSTATE_WM_HZ_OP | (5-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static void
gen8_emit_null_state(struct intel_bb *ibb) {
	gen8_emit_wm_hz_op(ibb);
	gen8_emit_hs(ibb);
	intel_bb_out(ibb, GEN7_3DSTATE_TE | (4-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	gen8_emit_gs(ibb);
	gen8_emit_ds(ibb);
	gen8_emit_vs(ibb);
}

static void
gen7_emit_clip(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN6_3DSTATE_CLIP | (4 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0); /*  pass-through */
	intel_bb_out(ibb, 0);
}

static void
gen8_emit_sf(struct intel_bb *ibb)
{
	int i;

	intel_bb_out(ibb, GEN7_3DSTATE_SBE | (4 - 2));
	intel_bb_out(ibb, 1 << GEN7_SBE_NUM_OUTPUTS_SHIFT |
		     GEN8_SBE_FORCE_URB_ENTRY_READ_LENGTH |
		     GEN8_SBE_FORCE_URB_ENTRY_READ_OFFSET |
		     1 << GEN7_SBE_URB_ENTRY_READ_LENGTH_SHIFT |
		     1 << GEN8_SBE_URB_ENTRY_READ_OFFSET_SHIFT);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN8_3DSTATE_SBE_SWIZ | (11 - 2));
	for (i = 0; i < 8; i++)
		intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN8_3DSTATE_RASTER | (5 - 2));
	intel_bb_out(ibb, GEN8_RASTER_FRONT_WINDING_CCW | GEN8_RASTER_CULL_NONE);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN6_3DSTATE_SF | (4 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static void
gen8_emit_ps(struct intel_bb *ibb, uint32_t kernel) {
	const int max_threads = 63;

	intel_bb_out(ibb, GEN6_3DSTATE_WM | (2 - 2));
	intel_bb_out(ibb, /* XXX: I don't understand the BARYCENTRIC stuff, but it
		   * appears we need it to put our setup data in the place we
		   * expect (g6, see below) */
		     GEN8_3DSTATE_PS_PERSPECTIVE_PIXEL_BARYCENTRIC);

	intel_bb_out(ibb, GEN6_3DSTATE_CONSTANT_PS | (11-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_PS | (12-2));
	intel_bb_out(ibb, kernel);
	intel_bb_out(ibb, 0); /* kernel hi */
	intel_bb_out(ibb, 1 << GEN6_3DSTATE_WM_SAMPLER_COUNT_SHIFT |
		     2 << GEN6_3DSTATE_WM_BINDING_TABLE_ENTRY_COUNT_SHIFT);
	intel_bb_out(ibb, 0); /* scratch space stuff */
	intel_bb_out(ibb, 0); /* scratch hi */
	intel_bb_out(ibb, (max_threads - 1) << GEN8_3DSTATE_PS_MAX_THREADS_SHIFT |
		     GEN6_3DSTATE_WM_16_DISPATCH_ENABLE);
	intel_bb_out(ibb, 6 << GEN6_3DSTATE_WM_DISPATCH_START_GRF_0_SHIFT);
	intel_bb_out(ibb, 0); // kernel 1
	intel_bb_out(ibb, 0); /* kernel 1 hi */
	intel_bb_out(ibb, 0); // kernel 2
	intel_bb_out(ibb, 0); /* kernel 2 hi */

	intel_bb_out(ibb, GEN8_3DSTATE_PS_BLEND | (2 - 2));
	intel_bb_out(ibb, GEN8_PS_BLEND_HAS_WRITEABLE_RT);

	intel_bb_out(ibb, GEN8_3DSTATE_PS_EXTRA | (2 - 2));
	intel_bb_out(ibb, GEN8_PSX_PIXEL_SHADER_VALID | GEN8_PSX_ATTRIBUTE_ENABLE);
}

static void
gen8_emit_depth(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN8_3DSTATE_WM_DEPTH_STENCIL | (3 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_DEPTH_BUFFER | (8-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN8_3DSTATE_HIER_DEPTH_BUFFER | (5 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN8_3DSTATE_STENCIL_BUFFER | (5 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static void
gen7_emit_clear(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN7_3DSTATE_CLEAR_PARAMS | (3-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 1); // clear valid
}

static void
gen6_emit_drawing_rectangle(struct intel_bb *ibb, const struct intel_buf *dst)
{
	intel_bb_out(ibb, GEN4_3DSTATE_DRAWING_RECTANGLE | (4 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, (intel_buf_height(dst) - 1) << 16 | (intel_buf_width(dst) - 1));
	intel_bb_out(ibb, 0);
}

static void gen8_emit_vf_topology(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN8_3DSTATE_VF_TOPOLOGY);
	intel_bb_out(ibb, _3DPRIM_RECTLIST);
}

/* Vertex elements MUST be defined before this according to spec */
static void gen8_emit_primitive(struct intel_bb *ibb, uint32_t offset)
{
	intel_bb_out(ibb, GEN8_3DSTATE_VF_INSTANCING | (3 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN4_3DPRIMITIVE | (7-2));
	intel_bb_out(ibb, 0);	/* gen8+ ignore the topology type field */
	intel_bb_out(ibb, 3);	/* vertex count */
	intel_bb_out(ibb, 0);	/*  We're specifying this instead with offset in GEN6_3DSTATE_VERTEX_BUFFERS */
	intel_bb_out(ibb, 1);	/* single instance */
	intel_bb_out(ibb, 0);	/* start instance location */
	intel_bb_out(ibb, 0);	/* index buffer offset, ignored */
}

/* The general rule is if it's named gen6 it is directly copied from
 * gen6_render_copyfunc.
 *
 * This sets up most of the 3d pipeline, and most of that to NULL state. The
 * docs aren't specific about exactly what must be set up NULL, but the general
 * rule is we could be run at any time, and so the most state we set to NULL,
 * the better our odds of success.
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
 * The batch commands point to state within tthe batch, so all state offsets should be
 * 0 < offset < 4096. Both commands and state build upwards, and are constructed
 * in that order. This means too many batch commands can delete state if not
 * careful.
 *
 */

#define BATCH_STATE_SPLIT 2048

void gen8_render_copyfunc(struct intel_bb *ibb,
			  struct intel_buf *src,
			  unsigned int src_x, unsigned int src_y,
			  unsigned int width, unsigned int height,
			  struct intel_buf *dst,
			  unsigned int dst_x, unsigned int dst_y)
{
	uint32_t ps_sampler_state, ps_kernel_off, ps_binding_table;
	uint32_t scissor_state;
	uint32_t vertex_buffer;

	igt_assert(src->bpp == dst->bpp);

	intel_bb_flush_render(ibb);

	intel_bb_add_intel_buf(ibb, dst, true);
	intel_bb_add_intel_buf(ibb, src, false);

	intel_bb_ptr_set(ibb, BATCH_STATE_SPLIT);

	ps_binding_table  = gen8_bind_surfaces(ibb, src, dst);
	ps_sampler_state  = gen8_create_sampler(ibb);
	ps_kernel_off = gen8_fill_ps(ibb, ps_kernel, sizeof(ps_kernel));
	vertex_buffer = gen7_fill_vertex_buffer_data(ibb,
						     src,
						     src_x, src_y,
						     dst_x, dst_y,
						     width, height);
	cc.cc_state = gen6_create_cc_state(ibb);
	cc.blend_state = gen8_create_blend_state(ibb);
	viewport.cc_state = gen6_create_cc_viewport(ibb);
	viewport.sf_clip_state = gen7_create_sf_clip_viewport(ibb);
	scissor_state = gen6_create_scissor_rect(ibb);
	/* TODO: theree is other state which isn't setup */

	intel_bb_ptr_set(ibb, 0);

	/* Start emitting the commands. The order roughly follows the mesa blorp
	 * order */
	intel_bb_out(ibb, G4X_PIPELINE_SELECT | PIPELINE_SELECT_3D);

	gen8_emit_sip(ibb);

	gen7_emit_push_constants(ibb);

	gen8_emit_state_base_address(ibb);

	intel_bb_out(ibb, GEN7_3DSTATE_VIEWPORT_STATE_POINTERS_CC);
	intel_bb_out(ibb, viewport.cc_state);
	intel_bb_out(ibb, GEN8_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP);
	intel_bb_out(ibb, viewport.sf_clip_state);

	gen7_emit_urb(ibb);

	gen8_emit_cc(ibb);

	gen8_emit_multisample(ibb);

	gen8_emit_null_state(ibb);

	intel_bb_out(ibb, GEN7_3DSTATE_STREAMOUT | (5-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	gen7_emit_clip(ibb);

	gen8_emit_sf(ibb);

	intel_bb_out(ibb, GEN7_3DSTATE_BINDING_TABLE_POINTERS_PS);
	intel_bb_out(ibb, ps_binding_table);

	intel_bb_out(ibb, GEN7_3DSTATE_SAMPLER_STATE_POINTERS_PS);
	intel_bb_out(ibb, ps_sampler_state);

	gen8_emit_ps(ibb, ps_kernel_off);

	intel_bb_out(ibb, GEN8_3DSTATE_SCISSOR_STATE_POINTERS);
	intel_bb_out(ibb, scissor_state);

	gen8_emit_depth(ibb);

	gen7_emit_clear(ibb);

	gen6_emit_drawing_rectangle(ibb, dst);

	gen8_emit_vertex_buffer(ibb, vertex_buffer);
	gen6_emit_vertex_elements(ibb);

	gen8_emit_vf_topology(ibb);
	gen8_emit_primitive(ibb, vertex_buffer);

	intel_bb_emit_bbe(ibb);
	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, false);
	dump_batch(ibb);
	intel_bb_reset(ibb, false);
}
