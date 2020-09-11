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

#include "i830_reg.h"
#include "rendercopy.h"

#define TB0C_LAST_STAGE	(1 << 31)
#define TB0C_RESULT_SCALE_1X		(0 << 29)
#define TB0C_RESULT_SCALE_2X		(1 << 29)
#define TB0C_RESULT_SCALE_4X		(2 << 29)
#define TB0C_OP_ARG1			(1 << 25)
#define TB0C_OP_MODULE			(3 << 25)
#define TB0C_OUTPUT_WRITE_CURRENT	(0 << 24)
#define TB0C_OUTPUT_WRITE_ACCUM		(1 << 24)
#define TB0C_ARG3_REPLICATE_ALPHA 	(1<<23)
#define TB0C_ARG3_INVERT		(1<<22)
#define TB0C_ARG3_SEL_XXX
#define TB0C_ARG2_REPLICATE_ALPHA 	(1<<17)
#define TB0C_ARG2_INVERT		(1<<16)
#define TB0C_ARG2_SEL_ONE		(0 << 12)
#define TB0C_ARG2_SEL_FACTOR		(1 << 12)
#define TB0C_ARG2_SEL_TEXEL0		(6 << 12)
#define TB0C_ARG2_SEL_TEXEL1		(7 << 12)
#define TB0C_ARG2_SEL_TEXEL2		(8 << 12)
#define TB0C_ARG2_SEL_TEXEL3		(9 << 12)
#define TB0C_ARG1_REPLICATE_ALPHA 	(1<<11)
#define TB0C_ARG1_INVERT		(1<<10)
#define TB0C_ARG1_SEL_ONE		(0 << 6)
#define TB0C_ARG1_SEL_TEXEL0		(6 << 6)
#define TB0C_ARG1_SEL_TEXEL1		(7 << 6)
#define TB0C_ARG1_SEL_TEXEL2		(8 << 6)
#define TB0C_ARG1_SEL_TEXEL3		(9 << 6)
#define TB0C_ARG0_REPLICATE_ALPHA 	(1<<5)
#define TB0C_ARG0_SEL_XXX

#define TB0A_CTR_STAGE_ENABLE 		(1<<31)
#define TB0A_RESULT_SCALE_1X		(0 << 29)
#define TB0A_RESULT_SCALE_2X		(1 << 29)
#define TB0A_RESULT_SCALE_4X		(2 << 29)
#define TB0A_OP_ARG1			(1 << 25)
#define TB0A_OP_MODULE			(3 << 25)
#define TB0A_OUTPUT_WRITE_CURRENT	(0<<24)
#define TB0A_OUTPUT_WRITE_ACCUM		(1<<24)
#define TB0A_CTR_STAGE_SEL_BITS_XXX
#define TB0A_ARG3_SEL_XXX
#define TB0A_ARG3_INVERT		(1<<17)
#define TB0A_ARG2_INVERT		(1<<16)
#define TB0A_ARG2_SEL_ONE		(0 << 12)
#define TB0A_ARG2_SEL_TEXEL0		(6 << 12)
#define TB0A_ARG2_SEL_TEXEL1		(7 << 12)
#define TB0A_ARG2_SEL_TEXEL2		(8 << 12)
#define TB0A_ARG2_SEL_TEXEL3		(9 << 12)
#define TB0A_ARG1_INVERT		(1<<10)
#define TB0A_ARG1_SEL_ONE		(0 << 6)
#define TB0A_ARG1_SEL_TEXEL0		(6 << 6)
#define TB0A_ARG1_SEL_TEXEL1		(7 << 6)
#define TB0A_ARG1_SEL_TEXEL2		(8 << 6)
#define TB0A_ARG1_SEL_TEXEL3		(9 << 6)


static void gen2_emit_invariant(struct intel_bb *ibb)
{
	int i;

	for (i = 0; i < 4; i++) {
		intel_bb_out(ibb, _3DSTATE_MAP_CUBE | MAP_UNIT(i));
		intel_bb_out(ibb, _3DSTATE_MAP_TEX_STREAM_CMD | MAP_UNIT(i) |
			     DISABLE_TEX_STREAM_BUMP |
			     ENABLE_TEX_STREAM_COORD_SET | TEX_STREAM_COORD_SET(i) |
			     ENABLE_TEX_STREAM_MAP_IDX | TEX_STREAM_MAP_IDX(i));
		intel_bb_out(ibb, _3DSTATE_MAP_COORD_TRANSFORM);
		intel_bb_out(ibb, DISABLE_TEX_TRANSFORM | TEXTURE_SET(i));
	}

	intel_bb_out(ibb, _3DSTATE_MAP_COORD_SETBIND_CMD);
	intel_bb_out(ibb, TEXBIND_SET3(TEXCOORDSRC_VTXSET_3) |
		     TEXBIND_SET2(TEXCOORDSRC_VTXSET_2) |
		     TEXBIND_SET1(TEXCOORDSRC_VTXSET_1) |
		     TEXBIND_SET0(TEXCOORDSRC_VTXSET_0));

	intel_bb_out(ibb, _3DSTATE_SCISSOR_ENABLE_CMD | DISABLE_SCISSOR_RECT);

	intel_bb_out(ibb, _3DSTATE_VERTEX_TRANSFORM);
	intel_bb_out(ibb, DISABLE_VIEWPORT_TRANSFORM | DISABLE_PERSPECTIVE_DIVIDE);

	intel_bb_out(ibb, _3DSTATE_W_STATE_CMD);
	intel_bb_out(ibb, MAGIC_W_STATE_DWORD1);
	intel_bb_out(ibb, 0x3f800000 /* 1.0 in IEEE float */);

	intel_bb_out(ibb, _3DSTATE_INDPT_ALPHA_BLEND_CMD |
		     DISABLE_INDPT_ALPHA_BLEND |
		     ENABLE_ALPHA_BLENDFUNC | ABLENDFUNC_ADD);

	intel_bb_out(ibb, _3DSTATE_CONST_BLEND_COLOR_CMD);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, _3DSTATE_MODES_1_CMD |
		     ENABLE_COLR_BLND_FUNC | BLENDFUNC_ADD |
		     ENABLE_SRC_BLND_FACTOR | SRC_BLND_FACT(BLENDFACTOR_ONE) |
		     ENABLE_DST_BLND_FACTOR | DST_BLND_FACT(BLENDFACTOR_ZERO));

	intel_bb_out(ibb, _3DSTATE_ENABLES_1_CMD |
		     DISABLE_LOGIC_OP |
		     DISABLE_STENCIL_TEST |
		     DISABLE_DEPTH_BIAS |
		     DISABLE_SPEC_ADD |
		     DISABLE_FOG |
		     DISABLE_ALPHA_TEST |
		     DISABLE_DEPTH_TEST |
		     ENABLE_COLOR_BLEND);

	intel_bb_out(ibb, _3DSTATE_ENABLES_2_CMD |
		     DISABLE_STENCIL_WRITE |
		     DISABLE_DITHER |
		     DISABLE_DEPTH_WRITE |
		     ENABLE_COLOR_MASK |
		     ENABLE_COLOR_WRITE |
		     ENABLE_TEX_CACHE);
}

static void gen2_emit_target(struct intel_bb *ibb,
			     const struct intel_buf *dst)
{
	uint32_t tiling;
	uint32_t format;

	igt_assert_lte(dst->surface[0].stride, 8192);
	igt_assert_lte(intel_buf_width(dst), 2048);
	igt_assert_lte(intel_buf_height(dst), 2048);

	switch (dst->bpp) {
		case 8: format = COLR_BUF_8BIT; break;
		case 16: format = COLR_BUF_RGB565; break;
		case 32: format = COLR_BUF_ARGB8888; break;
		default: igt_assert(0);
	}

	tiling = 0;
	if (dst->tiling != I915_TILING_NONE)
		tiling = BUF_3D_TILED_SURFACE;
	if (dst->tiling == I915_TILING_Y)
		tiling |= BUF_3D_TILE_WALK_Y;

	intel_bb_out(ibb, _3DSTATE_BUF_INFO_CMD);
	intel_bb_out(ibb, BUF_3D_ID_COLOR_BACK | tiling |
		     BUF_3D_PITCH(dst->surface[0].stride));
	intel_bb_emit_reloc(ibb, dst->handle, I915_GEM_DOMAIN_RENDER,
			    I915_GEM_DOMAIN_RENDER, 0, dst->addr.offset);

	intel_bb_out(ibb, _3DSTATE_DST_BUF_VARS_CMD);
	intel_bb_out(ibb, format |
		     DSTORG_HORT_BIAS(0x8) |
		     DSTORG_VERT_BIAS(0x8));

	intel_bb_out(ibb, _3DSTATE_DRAW_RECT_CMD);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);		/* ymin, xmin */
	intel_bb_out(ibb, DRAW_YMAX(intel_buf_height(dst) - 1) |
		     DRAW_XMAX(intel_buf_width(dst) - 1));
	intel_bb_out(ibb, 0);		/* yorig, xorig */
}

static void gen2_emit_texture(struct intel_bb *ibb,
			      const struct intel_buf *src,
			      int unit)
{
	uint32_t tiling;
	uint32_t format;

	igt_assert_lte(src->surface[0].stride, 8192);
	igt_assert_lte(intel_buf_width(src), 2048);
	igt_assert_lte(intel_buf_height(src), 2048);

	switch (src->bpp) {
		case 8: format = MAPSURF_8BIT | MT_8BIT_L8; break;
		case 16: format = MAPSURF_16BIT | MT_16BIT_RGB565; break;
		case 32: format = MAPSURF_32BIT | MT_32BIT_ARGB8888; break;
		default: igt_assert(0);
	}

	tiling = 0;
	if (src->tiling != I915_TILING_NONE)
		tiling = TM0S1_TILED_SURFACE;
	if (src->tiling == I915_TILING_Y)
		tiling |= TM0S1_TILE_WALK;

	intel_bb_out(ibb, _3DSTATE_LOAD_STATE_IMMEDIATE_2 | LOAD_TEXTURE_MAP(unit) | 4);
	intel_bb_emit_reloc(ibb, src->handle, I915_GEM_DOMAIN_SAMPLER, 0, 0,
			    src->addr.offset);
	intel_bb_out(ibb, (intel_buf_height(src) - 1) << TM0S1_HEIGHT_SHIFT |
		     (intel_buf_width(src) - 1) << TM0S1_WIDTH_SHIFT |
		     format | tiling);
	intel_bb_out(ibb, (src->surface[0].stride / 4 - 1) << TM0S2_PITCH_SHIFT | TM0S2_MAP_2D);
	intel_bb_out(ibb, FILTER_NEAREST << TM0S3_MAG_FILTER_SHIFT |
		     FILTER_NEAREST << TM0S3_MIN_FILTER_SHIFT |
		     MIPFILTER_NONE << TM0S3_MIP_FILTER_SHIFT);
	intel_bb_out(ibb, 0);	/* default color */

	intel_bb_out(ibb, _3DSTATE_MAP_COORD_SET_CMD | TEXCOORD_SET(unit) |
		     ENABLE_TEXCOORD_PARAMS | TEXCOORDS_ARE_NORMAL |
		     TEXCOORDTYPE_CARTESIAN |
		     ENABLE_ADDR_V_CNTL | TEXCOORD_ADDR_V_MODE(TEXCOORDMODE_CLAMP_BORDER) |
		     ENABLE_ADDR_U_CNTL | TEXCOORD_ADDR_U_MODE(TEXCOORDMODE_CLAMP_BORDER));
}

static void gen2_emit_copy_pipeline(struct intel_bb *ibb)
{
	intel_bb_out(ibb, _3DSTATE_INDPT_ALPHA_BLEND_CMD | DISABLE_INDPT_ALPHA_BLEND);
	intel_bb_out(ibb, _3DSTATE_ENABLES_1_CMD | DISABLE_LOGIC_OP |
		     DISABLE_STENCIL_TEST | DISABLE_DEPTH_BIAS |
		     DISABLE_SPEC_ADD | DISABLE_FOG | DISABLE_ALPHA_TEST |
		     DISABLE_COLOR_BLEND | DISABLE_DEPTH_TEST);

	intel_bb_out(ibb, _3DSTATE_LOAD_STATE_IMMEDIATE_2 |
		     LOAD_TEXTURE_BLEND_STAGE(0) | 1);
	intel_bb_out(ibb, TB0C_LAST_STAGE | TB0C_RESULT_SCALE_1X |
		     TB0C_OUTPUT_WRITE_CURRENT |
		     TB0C_OP_ARG1 | TB0C_ARG1_SEL_TEXEL0);
	intel_bb_out(ibb, TB0A_RESULT_SCALE_1X | TB0A_OUTPUT_WRITE_CURRENT |
		     TB0A_OP_ARG1 | TB0A_ARG1_SEL_TEXEL0);
}

void gen2_render_copyfunc(struct intel_bb *ibb,
			  uint32_t ctx,
			  struct intel_buf *src,
			  uint32_t src_x, uint32_t src_y,
			  uint32_t width, uint32_t height,
			  struct intel_buf *dst,
			  uint32_t dst_x, uint32_t dst_y)
{
	igt_assert(src->bpp == dst->bpp);

	intel_bb_add_intel_buf(ibb, dst, true);
	intel_bb_add_intel_buf(ibb, src, false);

	gen2_emit_invariant(ibb);
	gen2_emit_copy_pipeline(ibb);

	gen2_emit_target(ibb, dst);
	gen2_emit_texture(ibb, src, 0);

	intel_bb_out(ibb, _3DSTATE_LOAD_STATE_IMMEDIATE_1 |
		     I1_LOAD_S(2) | I1_LOAD_S(3) | I1_LOAD_S(8) | 2);
	intel_bb_out(ibb, 1<<12);
	intel_bb_out(ibb, S3_CULLMODE_NONE | S3_VERTEXHAS_XY);
	intel_bb_out(ibb, S8_ENABLE_COLOR_BUFFER_WRITE);

	intel_bb_out(ibb, _3DSTATE_VERTEX_FORMAT_2_CMD | TEXCOORDFMT_2D << 0);

	intel_bb_out(ibb, PRIM3D_INLINE | PRIM3D_RECTLIST | (3*4 - 1));
	emit_vertex(ibb, dst_x + width);
	emit_vertex(ibb, dst_y + height);
	emit_vertex_normalized(ibb, src_x + width, intel_buf_width(src));
	emit_vertex_normalized(ibb, src_y + height, intel_buf_height(src));

	emit_vertex(ibb, dst_x);
	emit_vertex(ibb, dst_y + height);
	emit_vertex_normalized(ibb, src_x, intel_buf_width(src));
	emit_vertex_normalized(ibb, src_y + height, intel_buf_height(src));

	emit_vertex(ibb, dst_x);
	emit_vertex(ibb, dst_y);
	emit_vertex_normalized(ibb, src_x, intel_buf_width(src));
	emit_vertex_normalized(ibb, src_y, intel_buf_height(src));

	intel_bb_flush_blit_with_context(ibb, ctx);
}
