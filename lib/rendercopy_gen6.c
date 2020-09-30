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
#include "drm.h"
#include "i915_drm.h"
#include "drmtest.h"
#include "intel_bufops.h"
#include "intel_batchbuffer.h"
#include "intel_io.h"
#include "rendercopy.h"
#include "gen6_render.h"
#include "intel_reg.h"

#define VERTEX_SIZE (3*4)

static const uint32_t ps_kernel_nomask_affine[][4] = {
	{ 0x0060005a, 0x204077be, 0x000000c0, 0x008d0040 },
	{ 0x0060005a, 0x206077be, 0x000000c0, 0x008d0080 },
	{ 0x0060005a, 0x208077be, 0x000000d0, 0x008d0040 },
	{ 0x0060005a, 0x20a077be, 0x000000d0, 0x008d0080 },
	{ 0x00000201, 0x20080061, 0x00000000, 0x00000000 },
	{ 0x00600001, 0x20200022, 0x008d0000, 0x00000000 },
	{ 0x02800031, 0x21c01cc9, 0x00000020, 0x0a8a0001 },
	{ 0x00600001, 0x204003be, 0x008d01c0, 0x00000000 },
	{ 0x00600001, 0x206003be, 0x008d01e0, 0x00000000 },
	{ 0x00600001, 0x208003be, 0x008d0200, 0x00000000 },
	{ 0x00600001, 0x20a003be, 0x008d0220, 0x00000000 },
	{ 0x00600001, 0x20c003be, 0x008d0240, 0x00000000 },
	{ 0x00600001, 0x20e003be, 0x008d0260, 0x00000000 },
	{ 0x00600001, 0x210003be, 0x008d0280, 0x00000000 },
	{ 0x00600001, 0x212003be, 0x008d02a0, 0x00000000 },
	{ 0x05800031, 0x24001cc8, 0x00000040, 0x90019000 },
	{ 0x0000007e, 0x00000000, 0x00000000, 0x00000000 },
	{ 0x0000007e, 0x00000000, 0x00000000, 0x00000000 },
	{ 0x0000007e, 0x00000000, 0x00000000, 0x00000000 },
	{ 0x0000007e, 0x00000000, 0x00000000, 0x00000000 },
	{ 0x0000007e, 0x00000000, 0x00000000, 0x00000000 },
	{ 0x0000007e, 0x00000000, 0x00000000, 0x00000000 },
	{ 0x0000007e, 0x00000000, 0x00000000, 0x00000000 },
	{ 0x0000007e, 0x00000000, 0x00000000, 0x00000000 },
};

static uint32_t
batch_round_upto(struct intel_bb *ibb, uint32_t divisor)
{
	uint32_t offset = intel_bb_offset(ibb);

	offset = (offset + divisor - 1) / divisor * divisor;
	intel_bb_ptr_set(ibb, offset);

	return offset;
}

static uint32_t
gen6_bind_buf(struct intel_bb *ibb, const struct intel_buf *buf, int is_dst)
{
	struct gen6_surface_state *ss;
	uint32_t write_domain, read_domain;
	uint64_t address;

	igt_assert_lte(buf->surface[0].stride, 128*1024);
	igt_assert_lte(intel_buf_width(buf), 8192);
	igt_assert_lte(intel_buf_height(buf), 8192);

	if (is_dst) {
		write_domain = read_domain = I915_GEM_DOMAIN_RENDER;
	} else {
		write_domain = 0;
		read_domain = I915_GEM_DOMAIN_SAMPLER;
	}

	ss = intel_bb_ptr_align(ibb, 32);
	ss->ss0.surface_type = SURFACE_2D;

	switch (buf->bpp) {
		case 8: ss->ss0.surface_format = SURFACEFORMAT_R8_UNORM; break;
		case 16: ss->ss0.surface_format = SURFACEFORMAT_R8G8_UNORM; break;
		case 32: ss->ss0.surface_format = SURFACEFORMAT_B8G8R8A8_UNORM; break;
		case 64: ss->ss0.surface_format = SURFACEFORMAT_R16G16B16A16_FLOAT; break;
		default: igt_assert(0);
	}

	ss->ss0.data_return_format = SURFACERETURNFORMAT_FLOAT32;
	ss->ss0.color_blend = 1;

	address = intel_bb_offset_reloc(ibb, buf->handle,
					read_domain, write_domain,
					intel_bb_offset(ibb) + 4,
					buf->addr.offset);
	ss->ss1.base_addr = (uint32_t) address;

	ss->ss2.height = intel_buf_height(buf) - 1;
	ss->ss2.width  = intel_buf_width(buf) - 1;
	ss->ss3.pitch  = buf->surface[0].stride - 1;
	ss->ss3.tiled_surface = buf->tiling != I915_TILING_NONE;
	ss->ss3.tile_walk     = buf->tiling == I915_TILING_Y;

	ss->ss5.memory_object_control = GEN6_MOCS_PTE;

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*ss));
}

static uint32_t
gen6_bind_surfaces(struct intel_bb *ibb,
		   const struct intel_buf *src,
		   const struct intel_buf *dst)
{
	uint32_t *binding_table, binding_table_offset;

	binding_table = intel_bb_ptr_align(ibb, 32);
	binding_table_offset = intel_bb_ptr_add_return_prev_offset(ibb, 32);

	binding_table[0] = gen6_bind_buf(ibb, dst, 1);
	binding_table[1] = gen6_bind_buf(ibb, src, 0);

	return binding_table_offset;
}

static void
gen6_emit_sip(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN4_STATE_SIP | 0);
	intel_bb_out(ibb, 0);
}

static void
gen6_emit_urb(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN6_3DSTATE_URB | (3 - 2));
	intel_bb_out(ibb, (1 - 1) << GEN6_3DSTATE_URB_VS_SIZE_SHIFT |
		     24 << GEN6_3DSTATE_URB_VS_ENTRIES_SHIFT); /* at least 24 on GEN6 */
	intel_bb_out(ibb, 0 << GEN6_3DSTATE_URB_GS_SIZE_SHIFT |
		     0 << GEN6_3DSTATE_URB_GS_ENTRIES_SHIFT); /* no GS thread */
}

static void
gen6_emit_state_base_address(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN4_STATE_BASE_ADDRESS | (10 - 2));
	intel_bb_out(ibb, 0); /* general */
	intel_bb_emit_reloc(ibb, ibb->handle, /* surface */
			    I915_GEM_DOMAIN_INSTRUCTION, 0,
			    BASE_ADDRESS_MODIFY, ibb->batch_offset);
	intel_bb_emit_reloc(ibb, ibb->handle, /* instruction */
			    I915_GEM_DOMAIN_INSTRUCTION, 0,
			    BASE_ADDRESS_MODIFY, ibb->batch_offset);
	intel_bb_out(ibb, 0); /* indirect */
	intel_bb_emit_reloc(ibb, ibb->handle, /* dynamic */
			    I915_GEM_DOMAIN_INSTRUCTION, 0,
			    BASE_ADDRESS_MODIFY, ibb->batch_offset);

	/* upper bounds, disable */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, BASE_ADDRESS_MODIFY);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, BASE_ADDRESS_MODIFY);
}

static void
gen6_emit_viewports(struct intel_bb *ibb, uint32_t cc_vp)
{
	intel_bb_out(ibb, GEN6_3DSTATE_VIEWPORT_STATE_POINTERS |
		     GEN6_3DSTATE_VIEWPORT_STATE_MODIFY_CC |
		     (4 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, cc_vp);
}

static void
gen6_emit_vs(struct intel_bb *ibb)
{
	/* disable VS constant buffer */
	intel_bb_out(ibb, GEN6_3DSTATE_CONSTANT_VS | (5 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN6_3DSTATE_VS | (6 - 2));
	intel_bb_out(ibb, 0); /* no VS kernel */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0); /* pass-through */
}

static void
gen6_emit_gs(struct intel_bb *ibb)
{
	/* disable GS constant buffer */
	intel_bb_out(ibb, GEN6_3DSTATE_CONSTANT_GS | (5 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN6_3DSTATE_GS | (7 - 2));
	intel_bb_out(ibb, 0); /* no GS kernel */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0); /* pass-through */
}

static void
gen6_emit_clip(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN6_3DSTATE_CLIP | (4 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0); /* pass-through */
	intel_bb_out(ibb, 0);
}

static void
gen6_emit_wm_constants(struct intel_bb *ibb)
{
	/* disable WM constant buffer */
	intel_bb_out(ibb, GEN6_3DSTATE_CONSTANT_PS | (5 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static void
gen6_emit_null_depth_buffer(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN4_3DSTATE_DEPTH_BUFFER | (7 - 2));
	intel_bb_out(ibb, SURFACE_NULL << GEN4_3DSTATE_DEPTH_BUFFER_TYPE_SHIFT |
		     GEN4_DEPTHFORMAT_D32_FLOAT << GEN4_3DSTATE_DEPTH_BUFFER_FORMAT_SHIFT);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN4_3DSTATE_CLEAR_PARAMS | (2 - 2));
	intel_bb_out(ibb, 0);
}

static void
gen6_emit_invariant(struct intel_bb *ibb)
{
	intel_bb_out(ibb, G4X_PIPELINE_SELECT | PIPELINE_SELECT_3D);

	intel_bb_out(ibb, GEN6_3DSTATE_MULTISAMPLE | (3 - 2));
	intel_bb_out(ibb, GEN6_3DSTATE_MULTISAMPLE_PIXEL_LOCATION_CENTER |
		     GEN6_3DSTATE_MULTISAMPLE_NUMSAMPLES_1); /* 1 sample/pixel */
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN6_3DSTATE_SAMPLE_MASK | (2 - 2));
	intel_bb_out(ibb, 1);
}

static void
gen6_emit_cc(struct intel_bb *ibb, uint32_t blend)
{
	intel_bb_out(ibb, GEN6_3DSTATE_CC_STATE_POINTERS | (4 - 2));
	intel_bb_out(ibb, blend | 1);
	intel_bb_out(ibb, 1024 | 1);
	intel_bb_out(ibb, 1024 | 1);
}

static void
gen6_emit_sampler(struct intel_bb *ibb, uint32_t state)
{
	intel_bb_out(ibb, GEN6_3DSTATE_SAMPLER_STATE_POINTERS |
		     GEN6_3DSTATE_SAMPLER_STATE_MODIFY_PS |
		     (4 - 2));
	intel_bb_out(ibb, 0); /* VS */
	intel_bb_out(ibb, 0); /* GS */
	intel_bb_out(ibb, state);
}

static void
gen6_emit_sf(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN6_3DSTATE_SF | (20 - 2));
	intel_bb_out(ibb, 1 << GEN6_3DSTATE_SF_NUM_OUTPUTS_SHIFT |
		     1 << GEN6_3DSTATE_SF_URB_ENTRY_READ_LENGTH_SHIFT |
		     1 << GEN6_3DSTATE_SF_URB_ENTRY_READ_OFFSET_SHIFT);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, GEN6_3DSTATE_SF_CULL_NONE);
	intel_bb_out(ibb, 2 << GEN6_3DSTATE_SF_TRIFAN_PROVOKE_SHIFT); /* DW4 */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0); /* DW9 */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0); /* DW14 */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0); /* DW19 */
}

static void
gen6_emit_wm(struct intel_bb *ibb, int kernel)
{
	intel_bb_out(ibb, GEN6_3DSTATE_WM | (9 - 2));
	intel_bb_out(ibb, kernel);
	intel_bb_out(ibb, 1 << GEN6_3DSTATE_WM_SAMPLER_COUNT_SHIFT |
		     2 << GEN6_3DSTATE_WM_BINDING_TABLE_ENTRY_COUNT_SHIFT);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 6 << GEN6_3DSTATE_WM_DISPATCH_START_GRF_0_SHIFT); /* DW4 */
	intel_bb_out(ibb, (40 - 1) << GEN6_3DSTATE_WM_MAX_THREADS_SHIFT |
		     GEN6_3DSTATE_WM_DISPATCH_ENABLE |
		     GEN6_3DSTATE_WM_16_DISPATCH_ENABLE);
	intel_bb_out(ibb, 1 << GEN6_3DSTATE_WM_NUM_SF_OUTPUTS_SHIFT |
		     GEN6_3DSTATE_WM_PERSPECTIVE_PIXEL_BARYCENTRIC);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static void
gen6_emit_binding_table(struct intel_bb *ibb, uint32_t wm_table)
{
	intel_bb_out(ibb, GEN4_3DSTATE_BINDING_TABLE_POINTERS |
		     GEN6_3DSTATE_BINDING_TABLE_MODIFY_PS |
		     (4 - 2));
	intel_bb_out(ibb, 0);		/* vs */
	intel_bb_out(ibb, 0);		/* gs */
	intel_bb_out(ibb, wm_table);
}

static void
gen6_emit_drawing_rectangle(struct intel_bb *ibb, const struct intel_buf *dst)
{
	intel_bb_out(ibb, GEN4_3DSTATE_DRAWING_RECTANGLE | (4 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, (intel_buf_height(dst) - 1) << 16 | (intel_buf_width(dst) - 1));
	intel_bb_out(ibb, 0);
}

static void
gen6_emit_vertex_elements(struct intel_bb *ibb)
{
	/* The VUE layout
	 *    dword 0-3: pad (0.0, 0.0, 0.0. 0.0)
	 *    dword 4-7: position (x, y, 1.0, 1.0),
	 *    dword 8-11: texture coordinate 0 (u0, v0, 0, 0)
	 *
	 * dword 4-11 are fetched from vertex buffer
	 */
	intel_bb_out(ibb, GEN4_3DSTATE_VERTEX_ELEMENTS | (2 * 3 + 1 - 2));

	intel_bb_out(ibb, 0 << GEN6_VE0_VERTEX_BUFFER_INDEX_SHIFT | GEN6_VE0_VALID |
		     SURFACEFORMAT_R32G32B32A32_FLOAT << VE0_FORMAT_SHIFT |
		     0 << VE0_OFFSET_SHIFT);
	intel_bb_out(ibb, GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_0_SHIFT |
		     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_1_SHIFT |
		     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT |
		     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_3_SHIFT);

	/* x,y */
	intel_bb_out(ibb, 0 << GEN6_VE0_VERTEX_BUFFER_INDEX_SHIFT | GEN6_VE0_VALID |
		     SURFACEFORMAT_R16G16_SSCALED << VE0_FORMAT_SHIFT |
		     0 << VE0_OFFSET_SHIFT); /* offsets vb in bytes */
	intel_bb_out(ibb, GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT |
		     GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT |
		     GEN4_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_2_SHIFT |
		     GEN4_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_3_SHIFT);

	/* u0, v0 */
	intel_bb_out(ibb, 0 << GEN6_VE0_VERTEX_BUFFER_INDEX_SHIFT | GEN6_VE0_VALID |
		     SURFACEFORMAT_R32G32_FLOAT << VE0_FORMAT_SHIFT |
		     4 << VE0_OFFSET_SHIFT);	/* offset vb in bytes */
	intel_bb_out(ibb, GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT |
		     GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT |
		     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT |
		     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_3_SHIFT);
}

static uint32_t
gen6_create_cc_viewport(struct intel_bb *ibb)
{
	struct gen4_cc_viewport *vp;

	vp = intel_bb_ptr_align(ibb, 32);

	vp->min_depth = -1.e35;
	vp->max_depth = 1.e35;

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*vp));
}

static uint32_t
gen6_create_cc_blend(struct intel_bb *ibb)
{
	struct gen6_blend_state *blend;

	blend = intel_bb_ptr_align(ibb, 64);

	blend->blend0.dest_blend_factor = GEN6_BLENDFACTOR_ZERO;
	blend->blend0.source_blend_factor = GEN6_BLENDFACTOR_ONE;
	blend->blend0.blend_func = GEN6_BLENDFUNCTION_ADD;
	blend->blend0.blend_enable = 1;

	blend->blend1.post_blend_clamp_enable = 1;
	blend->blend1.pre_blend_clamp_enable = 1;

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*blend));
}

static uint32_t
gen6_create_kernel(struct intel_bb *ibb)
{
	return intel_bb_copy_data(ibb, ps_kernel_nomask_affine,
				  sizeof(ps_kernel_nomask_affine), 64);
}

static uint32_t
gen6_create_sampler(struct intel_bb *ibb,
		    sampler_filter_t filter,
		    sampler_extend_t extend)
{
	struct gen6_sampler_state *ss;

	ss = intel_bb_ptr_align(ibb, 32);
	ss->ss0.lod_preclamp = 1;	/* GL mode */

	/* We use the legacy mode to get the semantics specified by
	 * the Render extension. */
	ss->ss0.border_color_mode = GEN4_BORDER_COLOR_MODE_LEGACY;

	switch (filter) {
	default:
	case SAMPLER_FILTER_NEAREST:
		ss->ss0.min_filter = GEN4_MAPFILTER_NEAREST;
		ss->ss0.mag_filter = GEN4_MAPFILTER_NEAREST;
		break;
	case SAMPLER_FILTER_BILINEAR:
		ss->ss0.min_filter = GEN4_MAPFILTER_LINEAR;
		ss->ss0.mag_filter = GEN4_MAPFILTER_LINEAR;
		break;
	}

	switch (extend) {
	default:
	case SAMPLER_EXTEND_NONE:
		ss->ss1.r_wrap_mode = GEN4_TEXCOORDMODE_CLAMP_BORDER;
		ss->ss1.s_wrap_mode = GEN4_TEXCOORDMODE_CLAMP_BORDER;
		ss->ss1.t_wrap_mode = GEN4_TEXCOORDMODE_CLAMP_BORDER;
		break;
	case SAMPLER_EXTEND_REPEAT:
		ss->ss1.r_wrap_mode = GEN4_TEXCOORDMODE_WRAP;
		ss->ss1.s_wrap_mode = GEN4_TEXCOORDMODE_WRAP;
		ss->ss1.t_wrap_mode = GEN4_TEXCOORDMODE_WRAP;
		break;
	case SAMPLER_EXTEND_PAD:
		ss->ss1.r_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;
		ss->ss1.s_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;
		ss->ss1.t_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;
		break;
	case SAMPLER_EXTEND_REFLECT:
		ss->ss1.r_wrap_mode = GEN4_TEXCOORDMODE_MIRROR;
		ss->ss1.s_wrap_mode = GEN4_TEXCOORDMODE_MIRROR;
		ss->ss1.t_wrap_mode = GEN4_TEXCOORDMODE_MIRROR;
		break;
	}

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*ss));
}

static void gen6_emit_vertex_buffer(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN4_3DSTATE_VERTEX_BUFFERS | 3);
	intel_bb_out(ibb, GEN6_VB0_VERTEXDATA |
		     0 << GEN6_VB0_BUFFER_INDEX_SHIFT |
		     VERTEX_SIZE << VB0_BUFFER_PITCH_SHIFT);
	intel_bb_emit_reloc(ibb, ibb->handle, I915_GEM_DOMAIN_VERTEX, 0,
			    0, ibb->batch_offset);
	intel_bb_emit_reloc(ibb, ibb->handle, I915_GEM_DOMAIN_VERTEX, 0,
			    ibb->size - 1, ibb->batch_offset);
	intel_bb_out(ibb, 0);
}

static uint32_t gen6_emit_primitive(struct intel_bb *ibb)
{
	uint32_t offset;

	intel_bb_out(ibb, GEN4_3DPRIMITIVE |
		     GEN4_3DPRIMITIVE_VERTEX_SEQUENTIAL |
		     _3DPRIM_RECTLIST << GEN4_3DPRIMITIVE_TOPOLOGY_SHIFT |
		     0 << 9 |
		     4);
	intel_bb_out(ibb, 3);	/* vertex count */
	offset = intel_bb_offset(ibb);
	intel_bb_out(ibb, 0);	/* vertex_index */
	intel_bb_out(ibb, 1);	/* single instance */
	intel_bb_out(ibb, 0);	/* start instance location */
	intel_bb_out(ibb, 0);	/* index buffer offset, ignored */

	return offset;
}

void gen6_render_copyfunc(struct intel_bb *ibb,
			  struct intel_buf *src,
			  uint32_t src_x, uint32_t src_y,
			  uint32_t width, uint32_t height,
			  struct intel_buf *dst,
			  uint32_t dst_x, uint32_t dst_y)
{
	uint32_t wm_state, wm_kernel, wm_table;
	uint32_t cc_vp, cc_blend, offset;
	uint32_t batch_end;

	igt_assert(src->bpp == dst->bpp);

	intel_bb_flush_render(ibb);

	intel_bb_add_intel_buf(ibb, dst, true);
	intel_bb_add_intel_buf(ibb, src, false);

	intel_bb_ptr_set(ibb, 1024 + 64);

	wm_table  = gen6_bind_surfaces(ibb, src, dst);
	wm_kernel = gen6_create_kernel(ibb);
	wm_state  = gen6_create_sampler(ibb,
					SAMPLER_FILTER_NEAREST,
					SAMPLER_EXTEND_NONE);

	cc_vp = gen6_create_cc_viewport(ibb);
	cc_blend = gen6_create_cc_blend(ibb);

	intel_bb_ptr_set(ibb, 0);

	gen6_emit_invariant(ibb);
	gen6_emit_state_base_address(ibb);

	gen6_emit_sip(ibb);
	gen6_emit_urb(ibb);

	gen6_emit_viewports(ibb, cc_vp);
	gen6_emit_vs(ibb);
	gen6_emit_gs(ibb);
	gen6_emit_clip(ibb);
	gen6_emit_wm_constants(ibb);
	gen6_emit_null_depth_buffer(ibb);

	gen6_emit_drawing_rectangle(ibb, dst);
	gen6_emit_cc(ibb, cc_blend);
	gen6_emit_sampler(ibb, wm_state);
	gen6_emit_sf(ibb);
	gen6_emit_wm(ibb, wm_kernel);
	gen6_emit_vertex_elements(ibb);
	gen6_emit_binding_table(ibb, wm_table);

	gen6_emit_vertex_buffer(ibb);
	offset = gen6_emit_primitive(ibb);

	batch_end = intel_bb_emit_bbe(ibb);

	ibb->batch[offset / sizeof(uint32_t)] =
			batch_round_upto(ibb, VERTEX_SIZE)/VERTEX_SIZE;

	emit_vertex_2s(ibb, dst_x + width, dst_y + height);
	emit_vertex_normalized(ibb, src_x + width, intel_buf_width(src));
	emit_vertex_normalized(ibb, src_y + height, intel_buf_height(src));

	emit_vertex_2s(ibb, dst_x, dst_y + height);
	emit_vertex_normalized(ibb, src_x, intel_buf_width(src));
	emit_vertex_normalized(ibb, src_y + height, intel_buf_height(src));

	emit_vertex_2s(ibb, dst_x, dst_y);
	emit_vertex_normalized(ibb, src_x, intel_buf_width(src));
	emit_vertex_normalized(ibb, src_y, intel_buf_height(src));

	/* Position to valid batch end position for batch reuse */
	intel_bb_ptr_set(ibb, batch_end);

	intel_bb_exec(ibb, batch_end,
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, false);
	intel_bb_reset(ibb, false);
}
