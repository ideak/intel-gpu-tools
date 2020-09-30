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

#include "i915_reg.h"
#include "i915_3d.h"
#include "rendercopy.h"

void gen3_render_copyfunc(struct intel_bb *ibb,
			  struct intel_buf *src,
			  uint32_t src_x, uint32_t src_y,
			  uint32_t width, uint32_t height,
			  struct intel_buf *dst,
			  uint32_t dst_x, uint32_t dst_y)
{
	igt_assert(src->bpp == dst->bpp);

	intel_bb_add_intel_buf(ibb, dst, true);
	intel_bb_add_intel_buf(ibb, src, false);

	/* invariant state */
	{
		intel_bb_out(ibb, _3DSTATE_AA_CMD |
			     AA_LINE_ECAAR_WIDTH_ENABLE |
			     AA_LINE_ECAAR_WIDTH_1_0 |
			     AA_LINE_REGION_WIDTH_ENABLE | AA_LINE_REGION_WIDTH_1_0);
		intel_bb_out(ibb, _3DSTATE_INDEPENDENT_ALPHA_BLEND_CMD |
			     IAB_MODIFY_ENABLE |
			     IAB_MODIFY_FUNC | (BLENDFUNC_ADD << IAB_FUNC_SHIFT) |
			     IAB_MODIFY_SRC_FACTOR | (BLENDFACT_ONE <<
						      IAB_SRC_FACTOR_SHIFT) |
			     IAB_MODIFY_DST_FACTOR | (BLENDFACT_ZERO <<
						      IAB_DST_FACTOR_SHIFT));
		intel_bb_out(ibb, _3DSTATE_DFLT_DIFFUSE_CMD);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, _3DSTATE_DFLT_SPEC_CMD);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, _3DSTATE_DFLT_Z_CMD);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, _3DSTATE_COORD_SET_BINDINGS |
			     CSB_TCB(0, 0) |
			     CSB_TCB(1, 1) |
			     CSB_TCB(2, 2) |
			     CSB_TCB(3, 3) |
			     CSB_TCB(4, 4) |
			     CSB_TCB(5, 5) | CSB_TCB(6, 6) | CSB_TCB(7, 7));
		intel_bb_out(ibb, _3DSTATE_RASTER_RULES_CMD |
			     ENABLE_POINT_RASTER_RULE |
			     OGL_POINT_RASTER_RULE |
			     ENABLE_LINE_STRIP_PROVOKE_VRTX |
			     ENABLE_TRI_FAN_PROVOKE_VRTX |
			     LINE_STRIP_PROVOKE_VRTX(1) |
			     TRI_FAN_PROVOKE_VRTX(2) | ENABLE_TEXKILL_3D_4D | TEXKILL_4D);
		intel_bb_out(ibb, _3DSTATE_MODES_4_CMD |
			     ENABLE_LOGIC_OP_FUNC | LOGIC_OP_FUNC(LOGICOP_COPY) |
			     ENABLE_STENCIL_WRITE_MASK | STENCIL_WRITE_MASK(0xff) |
			     ENABLE_STENCIL_TEST_MASK | STENCIL_TEST_MASK(0xff));
		intel_bb_out(ibb, _3DSTATE_LOAD_STATE_IMMEDIATE_1 | I1_LOAD_S(3) | I1_LOAD_S(4) | I1_LOAD_S(5) | 2);
		intel_bb_out(ibb, 0x00000000);	/* Disable texture coordinate wrap-shortest */
		intel_bb_out(ibb, (1 << S4_POINT_WIDTH_SHIFT) |
			     S4_LINE_WIDTH_ONE |
			     S4_CULLMODE_NONE |
			     S4_VFMT_XY);
		intel_bb_out(ibb, 0x00000000);	/* Stencil. */
		intel_bb_out(ibb, _3DSTATE_SCISSOR_ENABLE_CMD | DISABLE_SCISSOR_RECT);
		intel_bb_out(ibb, _3DSTATE_SCISSOR_RECT_0_CMD);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, _3DSTATE_DEPTH_SUBRECT_DISABLE);
		intel_bb_out(ibb, _3DSTATE_LOAD_INDIRECT | 0);	/* disable indirect state */
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, _3DSTATE_STIPPLE);
		intel_bb_out(ibb, 0x00000000);
		intel_bb_out(ibb, _3DSTATE_BACKFACE_STENCIL_OPS | BFO_ENABLE_STENCIL_TWO_SIDE | 0);
	}

	/* samler state */
	{
#define TEX_COUNT 1
		uint32_t format_bits, tiling_bits = 0;

		igt_assert_lte(src->surface[0].stride, 8192);
		igt_assert_lte(intel_buf_width(src), 2048);
		igt_assert_lte(intel_buf_height(src), 2048);

		if (src->tiling != I915_TILING_NONE)
			tiling_bits = MS3_TILED_SURFACE;
		if (src->tiling == I915_TILING_Y)
			tiling_bits |= MS3_TILE_WALK;

		switch (src->bpp) {
			case 8: format_bits = MAPSURF_8BIT | MT_8BIT_L8; break;
			case 16: format_bits = MAPSURF_16BIT | MT_16BIT_RGB565; break;
			case 32: format_bits = MAPSURF_32BIT | MT_32BIT_ARGB8888; break;
			default: igt_assert(0);
		}

		intel_bb_out(ibb, _3DSTATE_MAP_STATE | (3 * TEX_COUNT));
		intel_bb_out(ibb, (1 << TEX_COUNT) - 1);
		intel_bb_emit_reloc(ibb, src->handle,
				    I915_GEM_DOMAIN_SAMPLER, 0,
				    0, src->addr.offset);
		intel_bb_out(ibb, format_bits | tiling_bits |
			     (intel_buf_height(src) - 1) << MS3_HEIGHT_SHIFT |
			     (intel_buf_width(src) - 1) << MS3_WIDTH_SHIFT);
		intel_bb_out(ibb, (src->surface[0].stride/4-1) << MS4_PITCH_SHIFT);

		intel_bb_out(ibb, _3DSTATE_SAMPLER_STATE | (3 * TEX_COUNT));
		intel_bb_out(ibb, (1 << TEX_COUNT) - 1);
		intel_bb_out(ibb, MIPFILTER_NONE << SS2_MIP_FILTER_SHIFT |
			     FILTER_NEAREST << SS2_MAG_FILTER_SHIFT |
			     FILTER_NEAREST << SS2_MIN_FILTER_SHIFT);
		intel_bb_out(ibb, TEXCOORDMODE_WRAP << SS3_TCX_ADDR_MODE_SHIFT |
			     TEXCOORDMODE_WRAP << SS3_TCY_ADDR_MODE_SHIFT |
			     0 << SS3_TEXTUREMAP_INDEX_SHIFT);
		intel_bb_out(ibb, 0x00000000);
	}

	/* render target state */
	{
		uint32_t tiling_bits = 0;
		uint32_t format_bits;

		igt_assert_lte(dst->surface[0].stride, 8192);
		igt_assert_lte(intel_buf_width(dst), 2048);
		igt_assert_lte(intel_buf_height(dst), 2048);

		switch (dst->bpp) {
			case 8: format_bits = COLR_BUF_8BIT; break;
			case 16: format_bits = COLR_BUF_RGB565; break;
			case 32: format_bits = COLR_BUF_ARGB8888; break;
			default: igt_assert(0);
		}

		if (dst->tiling != I915_TILING_NONE)
			tiling_bits = BUF_3D_TILED_SURFACE;
		if (dst->tiling == I915_TILING_Y)
			tiling_bits |= BUF_3D_TILE_WALK_Y;

		intel_bb_out(ibb, _3DSTATE_BUF_INFO_CMD);
		intel_bb_out(ibb, BUF_3D_ID_COLOR_BACK | tiling_bits |
			     BUF_3D_PITCH(dst->surface[0].stride));
		intel_bb_emit_reloc(ibb, dst->handle,
				    I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER,
				    0, dst->addr.offset);

		intel_bb_out(ibb, _3DSTATE_DST_BUF_VARS_CMD);
		intel_bb_out(ibb, format_bits |
			     DSTORG_HORT_BIAS(0x8) |
			     DSTORG_VERT_BIAS(0x8));

		/* draw rect is unconditional */
		intel_bb_out(ibb, _3DSTATE_DRAW_RECT_CMD);
		intel_bb_out(ibb, 0x00000000);
		intel_bb_out(ibb, 0x00000000);	/* ymin, xmin */
		intel_bb_out(ibb, DRAW_YMAX(intel_buf_height(dst) - 1) |
			     DRAW_XMAX(intel_buf_width(dst) - 1));
		/* yorig, xorig (relate to color buffer?) */
		intel_bb_out(ibb, 0x00000000);
	}

	/* texfmt */
	{
		intel_bb_out(ibb, _3DSTATE_LOAD_STATE_IMMEDIATE_1 |
			     I1_LOAD_S(1) | I1_LOAD_S(2) | I1_LOAD_S(6) | 2);
		intel_bb_out(ibb, (4 << S1_VERTEX_WIDTH_SHIFT) |
			     (4 << S1_VERTEX_PITCH_SHIFT));
		intel_bb_out(ibb, ~S2_TEXCOORD_FMT(0, TEXCOORDFMT_NOT_PRESENT) | S2_TEXCOORD_FMT(0, TEXCOORDFMT_2D));
		intel_bb_out(ibb, S6_CBUF_BLEND_ENABLE | S6_COLOR_WRITE_ENABLE |
			     BLENDFUNC_ADD << S6_CBUF_BLEND_FUNC_SHIFT |
			     BLENDFACT_ONE << S6_CBUF_SRC_BLEND_FACT_SHIFT |
			     BLENDFACT_ZERO << S6_CBUF_DST_BLEND_FACT_SHIFT);
	}

	/* frage shader */
	{
		intel_bb_out(ibb, _3DSTATE_PIXEL_SHADER_PROGRAM | (1 + 3*3 - 2));
		/* decl FS_T0 */
		intel_bb_out(ibb, D0_DCL |
			     REG_TYPE(FS_T0) << D0_TYPE_SHIFT |
			     REG_NR(FS_T0) << D0_NR_SHIFT |
			     ((REG_TYPE(FS_T0) != REG_TYPE_S) ? D0_CHANNEL_ALL : 0));
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);
		/* decl FS_S0 */
		intel_bb_out(ibb, D0_DCL |
			     (REG_TYPE(FS_S0) << D0_TYPE_SHIFT) |
			     (REG_NR(FS_S0) << D0_NR_SHIFT) |
			     ((REG_TYPE(FS_S0) != REG_TYPE_S) ? D0_CHANNEL_ALL : 0));
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);
		/* texld(FS_OC, FS_S0, FS_T0 */
		intel_bb_out(ibb, T0_TEXLD |
			     (REG_TYPE(FS_OC) << T0_DEST_TYPE_SHIFT) |
			     (REG_NR(FS_OC) << T0_DEST_NR_SHIFT) |
			     (REG_NR(FS_S0) << T0_SAMPLER_NR_SHIFT));
		intel_bb_out(ibb, (REG_TYPE(FS_T0) << T1_ADDRESS_REG_TYPE_SHIFT) |
			     (REG_NR(FS_T0) << T1_ADDRESS_REG_NR_SHIFT));
		intel_bb_out(ibb, 0);
	}

	intel_bb_out(ibb, PRIM3D_RECTLIST | (3*4 - 1));
	emit_vertex(ibb, dst_x + width);
	emit_vertex(ibb, dst_y + height);
	emit_vertex(ibb, src_x + width);
	emit_vertex(ibb, src_y + height);

	emit_vertex(ibb, dst_x);
	emit_vertex(ibb, dst_y + height);
	emit_vertex(ibb, src_x);
	emit_vertex(ibb, src_y + height);

	emit_vertex(ibb, dst_x);
	emit_vertex(ibb, dst_y);
	emit_vertex(ibb, src_x);
	emit_vertex(ibb, src_y);

	intel_bb_flush_blit(ibb);
}
