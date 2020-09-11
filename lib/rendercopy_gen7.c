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
#include "intel_chipset.h"
#include "rendercopy.h"
#include "gen7_render.h"
#include "intel_reg.h"

#if DEBUG_RENDERCPY
static void dump_batch(struct intel_bb *ibb)
{
	intel_bb_dump(ibb, "/tmp/gen7-batchbuffers.dump");
}
#else
#define dump_batch(x) do { } while (0)
#endif

static const uint32_t ps_kernel[][4] = {
	{ 0x0080005a, 0x2e2077bd, 0x000000c0, 0x008d0040 },
	{ 0x0080005a, 0x2e6077bd, 0x000000d0, 0x008d0040 },
	{ 0x02800031, 0x21801fa9, 0x008d0e20, 0x08840001 },
	{ 0x00800001, 0x2e2003bd, 0x008d0180, 0x00000000 },
	{ 0x00800001, 0x2e6003bd, 0x008d01c0, 0x00000000 },
	{ 0x00800001, 0x2ea003bd, 0x008d0200, 0x00000000 },
	{ 0x00800001, 0x2ee003bd, 0x008d0240, 0x00000000 },
	{ 0x05800031, 0x20001fa8, 0x008d0e20, 0x90031000 },
};


static uint32_t
gen7_tiling_bits(uint32_t tiling)
{
	switch (tiling) {
	default: igt_assert(0);
	case I915_TILING_NONE: return 0;
	case I915_TILING_X: return GEN7_SURFACE_TILED;
	case I915_TILING_Y: return GEN7_SURFACE_TILED | GEN7_SURFACE_TILED_Y;
	}
}

static uint32_t
gen7_bind_buf(struct intel_bb *ibb,
	      const struct intel_buf *buf,
	      int is_dst)
{
	uint32_t format, *ss;
	uint32_t write_domain, read_domain;
	uint64_t address;

	igt_assert_lte(buf->surface[0].stride, 256*1024);
	igt_assert_lte(intel_buf_width(buf), 16384);
	igt_assert_lte(intel_buf_height(buf), 16384);

	switch (buf->bpp) {
		case 8: format = SURFACEFORMAT_R8_UNORM; break;
		case 16: format = SURFACEFORMAT_R8G8_UNORM; break;
		case 32: format = SURFACEFORMAT_B8G8R8A8_UNORM; break;
		case 64: format = SURFACEFORMAT_R16G16B16A16_FLOAT; break;
		default: igt_assert(0);
	}

	if (is_dst) {
		write_domain = read_domain = I915_GEM_DOMAIN_RENDER;
	} else {
		write_domain = 0;
		read_domain = I915_GEM_DOMAIN_SAMPLER;
	}

	ss = intel_bb_ptr_align(ibb, 32);

	ss[0] = (SURFACE_2D << GEN7_SURFACE_TYPE_SHIFT |
		 gen7_tiling_bits(buf->tiling) |
		format << GEN7_SURFACE_FORMAT_SHIFT);

	address = intel_bb_offset_reloc(ibb, buf->handle,
					read_domain, write_domain,
					intel_bb_offset(ibb) + 4,
					buf->addr.offset);
	ss[1] = address;
	ss[2] = ((intel_buf_width(buf) - 1)  << GEN7_SURFACE_WIDTH_SHIFT |
		 (intel_buf_height(buf) - 1) << GEN7_SURFACE_HEIGHT_SHIFT);
	ss[3] = (buf->surface[0].stride - 1) << GEN7_SURFACE_PITCH_SHIFT;
	ss[4] = 0;
	if (IS_VALLEYVIEW(ibb->devid))
		ss[5] = VLV_MOCS_L3 << 16;
	else
		ss[5] = (IVB_MOCS_L3 | IVB_MOCS_PTE) << 16;
	ss[6] = 0;
	ss[7] = 0;
	if (IS_HASWELL(ibb->devid))
		ss[7] |= HSW_SURFACE_SWIZZLE(RED, GREEN, BLUE, ALPHA);

	return intel_bb_ptr_add_return_prev_offset(ibb, 8 * sizeof(*ss));
}

static void
gen7_emit_vertex_elements(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN4_3DSTATE_VERTEX_ELEMENTS |
		     ((2 * (1 + 2)) + 1 - 2));

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
		     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT |
		     GEN4_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_3_SHIFT);

	/* s,t */
	intel_bb_out(ibb, 0 << GEN6_VE0_VERTEX_BUFFER_INDEX_SHIFT | GEN6_VE0_VALID |
		     SURFACEFORMAT_R16G16_SSCALED << VE0_FORMAT_SHIFT |
		     4 << VE0_OFFSET_SHIFT);  /* offset vb in bytes */
	intel_bb_out(ibb, GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT |
		     GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT |
		     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT |
		     GEN4_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_3_SHIFT);
}

static uint32_t
gen7_create_vertex_buffer(struct intel_bb *ibb,
			  uint32_t src_x, uint32_t src_y,
			  uint32_t dst_x, uint32_t dst_y,
			  uint32_t width, uint32_t height)
{
	uint16_t *v;

	v = intel_bb_ptr_align(ibb, 8);

	v[0] = dst_x + width;
	v[1] = dst_y + height;
	v[2] = src_x + width;
	v[3] = src_y + height;

	v[4] = dst_x;
	v[5] = dst_y + height;
	v[6] = src_x;
	v[7] = src_y + height;

	v[8] = dst_x;
	v[9] = dst_y;
	v[10] = src_x;
	v[11] = src_y;

	return intel_bb_ptr_add_return_prev_offset(ibb, 12 * sizeof(*v));
}

static void gen7_emit_vertex_buffer(struct intel_bb *ibb,
				    int src_x, int src_y,
				    int dst_x, int dst_y,
				    int width, int height,
				    uint32_t offset)
{
	intel_bb_out(ibb, GEN4_3DSTATE_VERTEX_BUFFERS | (5 - 2));
	intel_bb_out(ibb, 0 << GEN6_VB0_BUFFER_INDEX_SHIFT |
		     GEN6_VB0_VERTEXDATA |
		     GEN7_VB0_ADDRESS_MODIFY_ENABLE |
		     4 * 2 << VB0_BUFFER_PITCH_SHIFT);

	intel_bb_emit_reloc(ibb, ibb->handle, I915_GEM_DOMAIN_VERTEX, 0,
			    offset, ibb->batch_offset);
	intel_bb_out(ibb, ~0);
	intel_bb_out(ibb, 0);
}

static uint32_t
gen7_bind_surfaces(struct intel_bb *ibb,
		   const struct intel_buf *src,
		   const struct intel_buf *dst)
{
	uint32_t *binding_table, binding_table_offset;

	binding_table = intel_bb_ptr_align(ibb, 32);
	binding_table_offset = intel_bb_ptr_add_return_prev_offset(ibb, 8);

	binding_table[0] = gen7_bind_buf(ibb, dst, 1);
	binding_table[1] = gen7_bind_buf(ibb, src, 0);

	return binding_table_offset;
}

static void
gen7_emit_binding_table(struct intel_bb *ibb,
			const struct intel_buf *src,
			const struct intel_buf *dst,
			uint32_t bind_surf_off)
{
	intel_bb_out(ibb, GEN7_3DSTATE_BINDING_TABLE_POINTERS_PS | (2 - 2));
	intel_bb_out(ibb, bind_surf_off);
}

static void
gen7_emit_drawing_rectangle(struct intel_bb *ibb, const struct intel_buf *dst)
{
	intel_bb_out(ibb, GEN4_3DSTATE_DRAWING_RECTANGLE | (4 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, (intel_buf_height(dst) - 1) << 16 | (intel_buf_width(dst) - 1));
	intel_bb_out(ibb, 0);
}

static uint32_t
gen7_create_blend_state(struct intel_bb *ibb)
{
	struct gen6_blend_state *blend;

	blend = intel_bb_ptr_align(ibb, 64);

	blend->blend0.dest_blend_factor = GEN6_BLENDFACTOR_ZERO;
	blend->blend0.source_blend_factor = GEN6_BLENDFACTOR_ONE;
	blend->blend0.blend_func = GEN6_BLENDFUNCTION_ADD;
	blend->blend1.post_blend_clamp_enable = 1;
	blend->blend1.pre_blend_clamp_enable = 1;

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*blend));
}

static void
gen7_emit_state_base_address(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN4_STATE_BASE_ADDRESS | (10 - 2));
	intel_bb_out(ibb, 0);

	intel_bb_emit_reloc(ibb, ibb->handle, I915_GEM_DOMAIN_INSTRUCTION, 0,
			    BASE_ADDRESS_MODIFY,
			    ibb->batch_offset);
	intel_bb_emit_reloc(ibb, ibb->handle, I915_GEM_DOMAIN_INSTRUCTION, 0,
			    BASE_ADDRESS_MODIFY,
			    ibb->batch_offset);
	intel_bb_out(ibb, 0);
	intel_bb_emit_reloc(ibb, ibb->handle, I915_GEM_DOMAIN_INSTRUCTION, 0,
			    BASE_ADDRESS_MODIFY,
			    ibb->batch_offset);

	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);
}

static uint32_t
gen7_create_cc_viewport(struct intel_bb *ibb)
{
	struct gen4_cc_viewport *vp;

	vp = intel_bb_ptr_align(ibb, 32);
	vp->min_depth = -1.e35;
	vp->max_depth = 1.e35;

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*vp));
}

static void
gen7_emit_cc(struct intel_bb *ibb, uint32_t blend_state,
	     uint32_t cc_viewport)
{
	intel_bb_out(ibb, GEN7_3DSTATE_BLEND_STATE_POINTERS | (2 - 2));
	intel_bb_out(ibb, blend_state);

	intel_bb_out(ibb, GEN7_3DSTATE_VIEWPORT_STATE_POINTERS_CC | (2 - 2));
	intel_bb_out(ibb, cc_viewport);
}

static uint32_t
gen7_create_sampler(struct intel_bb *ibb)
{
	struct gen7_sampler_state *ss;

	ss = intel_bb_ptr_align(ibb, 32);

	ss->ss0.min_filter = GEN4_MAPFILTER_NEAREST;
	ss->ss0.mag_filter = GEN4_MAPFILTER_NEAREST;

	ss->ss3.r_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;
	ss->ss3.s_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;
	ss->ss3.t_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;

	ss->ss3.non_normalized_coord = 1;

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*ss));
}

static void
gen7_emit_sampler(struct intel_bb *ibb, uint32_t sampler_off)
{
	intel_bb_out(ibb, GEN7_3DSTATE_SAMPLER_STATE_POINTERS_PS | (2 - 2));
	intel_bb_out(ibb, sampler_off);
}

static void
gen7_emit_multisample(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN6_3DSTATE_MULTISAMPLE | (4 - 2));
	intel_bb_out(ibb, GEN6_3DSTATE_MULTISAMPLE_PIXEL_LOCATION_CENTER |
		     GEN6_3DSTATE_MULTISAMPLE_NUMSAMPLES_1); /* 1 sample/pixel */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN6_3DSTATE_SAMPLE_MASK | (2 - 2));
	intel_bb_out(ibb, 1);
}

static void
gen7_emit_urb(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_PS | (2 - 2));
	intel_bb_out(ibb, 8); /* in 1KBs */

	/* num of VS entries must be divisible by 8 if size < 9 */
	intel_bb_out(ibb, GEN7_3DSTATE_URB_VS | (2 - 2));
	intel_bb_out(ibb, (64 << GEN7_URB_ENTRY_NUMBER_SHIFT) |
		     (2 - 1) << GEN7_URB_ENTRY_SIZE_SHIFT |
		     (1 << GEN7_URB_STARTING_ADDRESS_SHIFT));

	intel_bb_out(ibb, GEN7_3DSTATE_URB_HS | (2 - 2));
	intel_bb_out(ibb, (0 << GEN7_URB_ENTRY_SIZE_SHIFT) |
		     (2 << GEN7_URB_STARTING_ADDRESS_SHIFT));

	intel_bb_out(ibb, GEN7_3DSTATE_URB_DS | (2 - 2));
	intel_bb_out(ibb, (0 << GEN7_URB_ENTRY_SIZE_SHIFT) |
		     (2 << GEN7_URB_STARTING_ADDRESS_SHIFT));

	intel_bb_out(ibb, GEN7_3DSTATE_URB_GS | (2 - 2));
	intel_bb_out(ibb, (0 << GEN7_URB_ENTRY_SIZE_SHIFT) |
		     (1 << GEN7_URB_STARTING_ADDRESS_SHIFT));
}

static void
gen7_emit_vs(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN6_3DSTATE_VS | (6 - 2));
	intel_bb_out(ibb, 0); /* no VS kernel */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0); /* pass-through */
}

static void
gen7_emit_hs(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN7_3DSTATE_HS | (7 - 2));
	intel_bb_out(ibb, 0); /* no HS kernel */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0); /* pass-through */
}

static void
gen7_emit_te(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN7_3DSTATE_TE | (4 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static void
gen7_emit_ds(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN7_3DSTATE_DS | (6 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static void
gen7_emit_gs(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN6_3DSTATE_GS | (7 - 2));
	intel_bb_out(ibb, 0); /* no GS kernel */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0); /* pass-through  */
}

static void
gen7_emit_streamout(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN7_3DSTATE_STREAMOUT | (3 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static void
gen7_emit_sf(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN6_3DSTATE_SF | (7 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, GEN6_3DSTATE_SF_CULL_NONE);
	intel_bb_out(ibb, 2 << GEN6_3DSTATE_SF_TRIFAN_PROVOKE_SHIFT);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static void
gen7_emit_sbe(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN7_3DSTATE_SBE | (14 - 2));
	intel_bb_out(ibb, 1 << GEN7_SBE_NUM_OUTPUTS_SHIFT |
		     1 << GEN7_SBE_URB_ENTRY_READ_LENGTH_SHIFT |
		     1 << GEN7_SBE_URB_ENTRY_READ_OFFSET_SHIFT);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0); /* dw4 */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0); /* dw8 */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0); /* dw12 */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static void
gen7_emit_ps(struct intel_bb *ibb, uint32_t kernel_off)
{
	int threads;

	if (IS_HASWELL(ibb->devid))
		threads = 40 << HSW_PS_MAX_THREADS_SHIFT | 1 << HSW_PS_SAMPLE_MASK_SHIFT;
	else
		threads = 40 << IVB_PS_MAX_THREADS_SHIFT;

	intel_bb_out(ibb, GEN7_3DSTATE_PS | (8 - 2));
	intel_bb_out(ibb, kernel_off);
	intel_bb_out(ibb, 1 << GEN7_PS_SAMPLER_COUNT_SHIFT |
		     2 << GEN7_PS_BINDING_TABLE_ENTRY_COUNT_SHIFT);
	intel_bb_out(ibb, 0); /* scratch address */
	intel_bb_out(ibb, threads |
		     GEN7_PS_16_DISPATCH_ENABLE |
		     GEN7_PS_ATTRIBUTE_ENABLE);
	intel_bb_out(ibb, 6 << GEN7_PS_DISPATCH_START_GRF_SHIFT_0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static void
gen7_emit_clip(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN6_3DSTATE_CLIP | (4 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0); /* pass-through */
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CL | (2 - 2));
	intel_bb_out(ibb, 0);
}

static void
gen7_emit_wm(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN6_3DSTATE_WM | (3 - 2));
	intel_bb_out(ibb, GEN7_WM_DISPATCH_ENABLE |
		     GEN7_WM_PERSPECTIVE_PIXEL_BARYCENTRIC);
	intel_bb_out(ibb, 0);
}

static void
gen7_emit_null_depth_buffer(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN7_3DSTATE_DEPTH_BUFFER | (7 - 2));
	intel_bb_out(ibb, SURFACE_NULL << GEN4_3DSTATE_DEPTH_BUFFER_TYPE_SHIFT |
		     GEN4_DEPTHFORMAT_D32_FLOAT << GEN4_3DSTATE_DEPTH_BUFFER_FORMAT_SHIFT);
	intel_bb_out(ibb, 0); /* disable depth, stencil and hiz */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_CLEAR_PARAMS | (3 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

#define BATCH_STATE_SPLIT 2048
void gen7_render_copyfunc(struct intel_bb *ibb,
			  uint32_t ctx,
			  struct intel_buf *src,
			  uint32_t src_x, uint32_t src_y,
			  uint32_t width, uint32_t height,
			  struct intel_buf *dst,
			  uint32_t dst_x, uint32_t dst_y)
{
	uint32_t ps_binding_table, ps_sampler_off, ps_kernel_off;
	uint32_t blend_state, cc_viewport;
	uint32_t vertex_buffer;

	igt_assert(src->bpp == dst->bpp);

	intel_bb_flush_render_with_context(ibb, ctx);

	intel_bb_add_intel_buf(ibb, dst, true);
	intel_bb_add_intel_buf(ibb, src, false);

	intel_bb_ptr_set(ibb, BATCH_STATE_SPLIT);

	blend_state = gen7_create_blend_state(ibb);
	cc_viewport = gen7_create_cc_viewport(ibb);
	ps_sampler_off = gen7_create_sampler(ibb);
	ps_kernel_off = intel_bb_copy_data(ibb, ps_kernel,
					   sizeof(ps_kernel), 64);
	vertex_buffer = gen7_create_vertex_buffer(ibb,
						  src_x, src_y,
						  dst_x, dst_y,
						  width, height);
	ps_binding_table = gen7_bind_surfaces(ibb, src, dst);

	intel_bb_ptr_set(ibb, 0);

	intel_bb_out(ibb, G4X_PIPELINE_SELECT | PIPELINE_SELECT_3D);

	gen7_emit_state_base_address(ibb);
	gen7_emit_multisample(ibb);
	gen7_emit_urb(ibb);
	gen7_emit_vs(ibb);
	gen7_emit_hs(ibb);
	gen7_emit_te(ibb);
	gen7_emit_ds(ibb);
	gen7_emit_gs(ibb);
	gen7_emit_clip(ibb);
	gen7_emit_sf(ibb);
	gen7_emit_wm(ibb);
	gen7_emit_streamout(ibb);
	gen7_emit_null_depth_buffer(ibb);
	gen7_emit_cc(ibb, blend_state, cc_viewport);
	gen7_emit_sampler(ibb, ps_sampler_off);
	gen7_emit_sbe(ibb);
	gen7_emit_ps(ibb, ps_kernel_off);
	gen7_emit_vertex_elements(ibb);
	gen7_emit_vertex_buffer(ibb, src_x, src_y,
				dst_x, dst_y, width,
				height, vertex_buffer);
	gen7_emit_binding_table(ibb, src, dst, ps_binding_table);
	gen7_emit_drawing_rectangle(ibb, dst);

	intel_bb_out(ibb, GEN4_3DPRIMITIVE | (7 - 2));
	intel_bb_out(ibb, GEN4_3DPRIMITIVE_VERTEX_SEQUENTIAL | _3DPRIM_RECTLIST);
	intel_bb_out(ibb, 3);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 1);   /* single instance */
	intel_bb_out(ibb, 0);   /* start instance location */
	intel_bb_out(ibb, 0);   /* index buffer offset, ignored */

	intel_bb_emit_bbe(ibb);
	intel_bb_exec_with_context(ibb, intel_bb_offset(ibb), ctx,
				   I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC,
				   false);
	dump_batch(ibb);
	intel_bb_reset(ibb, false);
}
