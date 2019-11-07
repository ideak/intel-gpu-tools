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
#include <getopt.h>

#include <drm.h>
#include <i915_drm.h>

#include "drmtest.h"
#include "intel_aux_pgtable.h"
#include "intel_bufmgr.h"
#include "intel_batchbuffer.h"
#include "intel_io.h"
#include "rendercopy.h"
#include "gen9_render.h"
#include "intel_reg.h"
#include "igt_aux.h"

#include "intel_aub.h"

#define VERTEX_SIZE (3*4)

#if DEBUG_RENDERCPY
static void dump_batch(struct intel_batchbuffer *batch) {
	int fd = open("/tmp/i965-batchbuffers.dump", O_WRONLY | O_CREAT,  0666);
	if (fd != -1) {
		igt_assert_eq(write(fd, batch->buffer, 4096), 4096);
		fd = close(fd);
	}
}
#else
#define dump_batch(x) do { } while(0)
#endif

struct {
	uint32_t cc_state;
	uint32_t blend_state;
} cc;

struct {
	uint32_t cc_state;
	uint32_t sf_clip_state;
} viewport;

/* see lib/i915/shaders/ps/blit.g7a */
static const uint32_t ps_kernel_gen9[][4] = {
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

/* see lib/i915/shaders/ps/blit.g11a */
static const uint32_t ps_kernel_gen11[][4] = {
#if 1
	{ 0x0060005b, 0x2000c01c, 0x07206601, 0x01800404 },
	{ 0x0060005b, 0x7100480c, 0x0722003b, 0x01880406 },
	{ 0x0060005b, 0x2000c01c, 0x07206601, 0x01800408 },
	{ 0x0060005b, 0x7200480c, 0x0722003b, 0x0188040a },
	{ 0x0060005b, 0x2000c01c, 0x07206e01, 0x01a00404 },
	{ 0x0060005b, 0x7300480c, 0x0722003b, 0x01a80406 },
	{ 0x0060005b, 0x2000c01c, 0x07206e01, 0x01a00408 },
	{ 0x0060005b, 0x7400480c, 0x0722003b, 0x01a8040a },
	{ 0x02800031, 0x21804a4c, 0x06000e20, 0x08840001 },
	{ 0x00800001, 0x2e204b28, 0x008d0180, 0x00000000 },
	{ 0x00800001, 0x2e604b28, 0x008d01c0, 0x00000000 },
	{ 0x00800001, 0x2ea04b28, 0x008d0200, 0x00000000 },
	{ 0x00800001, 0x2ee04b28, 0x008d0240, 0x00000000 },
	{ 0x05800031, 0x20004a44, 0x06000e20, 0x90031000 },
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

/* see lib/i915/shaders/ps/gen12_render_copy.asm */
static const uint32_t gen12_render_copy[][4] = {
	{ 0x8003005b, 0x200002f0, 0x0a0a0664, 0x06040205 },
	{ 0x8003005b, 0x71040fa8, 0x0a0a2001, 0x06240305 },
	{ 0x8003005b, 0x200002f0, 0x0a0a0664, 0x06040405 },
	{ 0x8003005b, 0x72040fa8, 0x0a0a2001, 0x06240505 },
	{ 0x8003005b, 0x200002f0, 0x0a0a06e4, 0x06840205 },
	{ 0x8003005b, 0x73040fa8, 0x0a0a2001, 0x06a40305 },
	{ 0x8003005b, 0x200002f0, 0x0a0a06e4, 0x06840405 },
	{ 0x8003005b, 0x74040fa8, 0x0a0a2001, 0x06a40505 },
	{ 0x80049031, 0x0c440000, 0x20027124, 0x01000000 },
	{ 0x00042061, 0x71050aa0, 0x00460c05, 0x00000000 },
	{ 0x00040061, 0x73050aa0, 0x00460e05, 0x00000000 },
	{ 0x00040061, 0x75050aa0, 0x00461005, 0x00000000 },
	{ 0x00040061, 0x77050aa0, 0x00461205, 0x00000000 },
	{ 0x80040131, 0x00000004, 0x50007144, 0x00c40000 },
};

/* AUB annotation support */
#define MAX_ANNOTATIONS	33
struct annotations_context {
	drm_intel_aub_annotation annotations[MAX_ANNOTATIONS];
	int index;
	uint32_t offset;
} aub_annotations;

static void annotation_init(struct annotations_context *ctx)
{
	/* ctx->annotations is an array keeping a list of annotations of the
	 * batch buffer ordered by offset. ctx->annotations[0] is thus left
	 * for the command stream and will be filled just before executing
	 * the batch buffer with annotations_add_batch() */
	ctx->index = 1;
}

static void add_annotation(drm_intel_aub_annotation *a,
			   uint32_t type, uint32_t subtype,
			   uint32_t ending_offset)
{
	a->type = type;
	a->subtype = subtype;
	a->ending_offset = ending_offset;
}

static void annotation_add_batch(struct annotations_context *ctx, size_t size)
{
	add_annotation(&ctx->annotations[0], AUB_TRACE_TYPE_BATCH, 0, size);
}

static void annotation_add_state(struct annotations_context *ctx,
				 uint32_t state_type,
				 uint32_t start_offset,
				 size_t   size)
{
	assert(ctx->index < MAX_ANNOTATIONS);

	add_annotation(&ctx->annotations[ctx->index++],
		       AUB_TRACE_TYPE_NOTYPE, 0,
		       start_offset);
	add_annotation(&ctx->annotations[ctx->index++],
		       AUB_TRACE_TYPE(state_type),
		       AUB_TRACE_SUBTYPE(state_type),
		       start_offset + size);
}

static void annotation_flush(struct annotations_context *ctx,
			     struct intel_batchbuffer *batch)
{
	if (!igt_aub_dump_enabled())
		return;

	drm_intel_bufmgr_gem_set_aub_annotations(batch->bo,
						 ctx->annotations,
						 ctx->index);
}

static void
gen6_render_flush(struct intel_batchbuffer *batch,
		  drm_intel_context *context, uint32_t batch_end)
{
	int ret;

	ret = drm_intel_bo_subdata(batch->bo, 0, 4096, batch->buffer);
	if (ret == 0)
		ret = drm_intel_gem_bo_context_exec(batch->bo, context,
						    batch_end, 0);
	assert(ret == 0);
}

/* Mostly copy+paste from gen6, except height, width, pitch moved */
static uint32_t
gen8_bind_buf(struct intel_batchbuffer *batch, const struct igt_buf *buf,
	      int is_dst) {
	struct gen9_surface_state *ss;
	uint32_t write_domain, read_domain, offset;
	int ret;

	igt_assert_lte(buf->stride, 256*1024);
	igt_assert_lte(igt_buf_width(buf), 16384);
	igt_assert_lte(igt_buf_height(buf), 16384);

	if (is_dst) {
		write_domain = read_domain = I915_GEM_DOMAIN_RENDER;
	} else {
		write_domain = 0;
		read_domain = I915_GEM_DOMAIN_SAMPLER;
	}

	ss = intel_batchbuffer_subdata_alloc(batch, sizeof(*ss), 64);
	offset = intel_batchbuffer_subdata_offset(batch, ss);
	annotation_add_state(&aub_annotations, AUB_TRACE_SURFACE_STATE,
			     offset, sizeof(*ss));

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
	else if (buf->tiling != I915_TILING_NONE)
		ss->ss0.tiled_mode = 3;

	ss->ss1.memory_object_control = I915_MOCS_PTE << 1;

	if (buf->tiling == I915_TILING_Yf)
		ss->ss5.trmode = 1;
	else if (buf->tiling == I915_TILING_Ys)
		ss->ss5.trmode = 2;
	ss->ss5.mip_tail_start_lod = 1; /* needed with trmode */

	ss->ss8.base_addr = buf->bo->offset64;
	ss->ss9.base_addr_hi = buf->bo->offset64 >> 32;

	ret = drm_intel_bo_emit_reloc(batch->bo,
				      intel_batchbuffer_subdata_offset(batch, &ss->ss8),
				      buf->bo, 0,
				      read_domain, write_domain);
	assert(ret == 0);

	ss->ss2.height = igt_buf_height(buf) - 1;
	ss->ss2.width  = igt_buf_width(buf) - 1;
	ss->ss3.pitch  = buf->stride - 1;

	ss->ss7.shader_chanel_select_r = 4;
	ss->ss7.shader_chanel_select_g = 5;
	ss->ss7.shader_chanel_select_b = 6;
	ss->ss7.shader_chanel_select_a = 7;

	if (buf->aux.stride) {
		ss->ss6.aux_mode = 0x5; /* AUX_CCS_E */
		ss->ss6.aux_pitch = (buf->aux.stride / 128) - 1;

		ss->ss10.aux_base_addr = buf->bo->offset64 + buf->aux.offset;
		ss->ss11.aux_base_addr_hi = (buf->bo->offset64 + buf->aux.offset) >> 32;

		ret = drm_intel_bo_emit_reloc(batch->bo,
					      intel_batchbuffer_subdata_offset(batch, &ss->ss10),
					      buf->bo, buf->aux.offset,
					      read_domain, write_domain);
		assert(ret == 0);
	}

	return offset;
}

static uint32_t
gen8_bind_surfaces(struct intel_batchbuffer *batch,
		   const struct igt_buf *src,
		   const struct igt_buf *dst)
{
	uint32_t *binding_table, offset;

	binding_table = intel_batchbuffer_subdata_alloc(batch, 8, 32);
	offset = intel_batchbuffer_subdata_offset(batch, binding_table);
	annotation_add_state(&aub_annotations, AUB_TRACE_BINDING_TABLE,
			     offset, 8);

	binding_table[0] = gen8_bind_buf(batch, dst, 1);
	binding_table[1] = gen8_bind_buf(batch, src, 0);

	return offset;
}

/* Mostly copy+paste from gen6, except wrap modes moved */
static uint32_t
gen8_create_sampler(struct intel_batchbuffer *batch) {
	struct gen8_sampler_state *ss;
	uint32_t offset;

	ss = intel_batchbuffer_subdata_alloc(batch, sizeof(*ss), 64);
	offset = intel_batchbuffer_subdata_offset(batch, ss);
	annotation_add_state(&aub_annotations, AUB_TRACE_SAMPLER_STATE,
			     offset, sizeof(*ss));

	ss->ss0.min_filter = GEN4_MAPFILTER_NEAREST;
	ss->ss0.mag_filter = GEN4_MAPFILTER_NEAREST;
	ss->ss3.r_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;
	ss->ss3.s_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;
	ss->ss3.t_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;

	/* I've experimented with non-normalized coordinates and using the LD
	 * sampler fetch, but couldn't make it work. */
	ss->ss3.non_normalized_coord = 0;

	return offset;
}

static uint32_t
gen8_fill_ps(struct intel_batchbuffer *batch,
	     const uint32_t kernel[][4],
	     size_t size)
{
	uint32_t offset;

	offset = intel_batchbuffer_copy_data(batch, kernel, size, 64);
	annotation_add_state(&aub_annotations, AUB_TRACE_KERNEL_INSTRUCTIONS,
			     offset, size);

	return offset;
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
gen7_fill_vertex_buffer_data(struct intel_batchbuffer *batch,
			     const struct igt_buf *src,
			     uint32_t src_x, uint32_t src_y,
			     uint32_t dst_x, uint32_t dst_y,
			     uint32_t width, uint32_t height)
{
	void *start;
	uint32_t offset;

	intel_batchbuffer_align(batch, 8);
	start = batch->ptr;

	emit_vertex_2s(batch, dst_x + width, dst_y + height);
	emit_vertex_normalized(batch, src_x + width, igt_buf_width(src));
	emit_vertex_normalized(batch, src_y + height, igt_buf_height(src));

	emit_vertex_2s(batch, dst_x, dst_y + height);
	emit_vertex_normalized(batch, src_x, igt_buf_width(src));
	emit_vertex_normalized(batch, src_y + height, igt_buf_height(src));

	emit_vertex_2s(batch, dst_x, dst_y);
	emit_vertex_normalized(batch, src_x, igt_buf_width(src));
	emit_vertex_normalized(batch, src_y, igt_buf_height(src));

	offset = intel_batchbuffer_subdata_offset(batch, start);
	annotation_add_state(&aub_annotations, AUB_TRACE_VERTEX_BUFFER,
			     offset, 3 * VERTEX_SIZE);
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
gen6_emit_vertex_elements(struct intel_batchbuffer *batch) {
	/*
	 * The VUE layout
	 *    dword 0-3: pad (0, 0, 0. 0)
	 *    dword 4-7: position (x, y, 0, 1.0),
	 *    dword 8-11: texture coordinate 0 (u0, v0, 0, 1.0)
	 */
	OUT_BATCH(GEN4_3DSTATE_VERTEX_ELEMENTS | (3 * 2 + 1 - 2));

	/* Element state 0. These are 4 dwords of 0 required for the VUE format.
	 * We don't really know or care what they do.
	 */
	OUT_BATCH(0 << GEN6_VE0_VERTEX_BUFFER_INDEX_SHIFT | GEN6_VE0_VALID |
		  SURFACEFORMAT_R32G32B32A32_FLOAT << VE0_FORMAT_SHIFT |
		  0 << VE0_OFFSET_SHIFT); /* we specify 0, but it's really does not exist */
	OUT_BATCH(GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_0_SHIFT |
		  GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_1_SHIFT |
		  GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT |
		  GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_3_SHIFT);

	/* Element state 1 - Our "destination" vertices. These are passed down
	 * through the pipeline, and eventually make it to the pixel shader as
	 * the offsets in the destination surface. It's packed as the 16
	 * signed/scaled because of gen6 rendercopy. I see no particular reason
	 * for doing this though.
	 */
	OUT_BATCH(0 << GEN6_VE0_VERTEX_BUFFER_INDEX_SHIFT | GEN6_VE0_VALID |
		  SURFACEFORMAT_R16G16_SSCALED << VE0_FORMAT_SHIFT |
		  0 << VE0_OFFSET_SHIFT); /* offsets vb in bytes */
	OUT_BATCH(GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT |
		  GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT |
		  GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT |
		  GEN4_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_3_SHIFT);

	/* Element state 2. Last but not least we store the U,V components as
	 * normalized floats. These will be used in the pixel shader to sample
	 * from the source buffer.
	 */
	OUT_BATCH(0 << GEN6_VE0_VERTEX_BUFFER_INDEX_SHIFT | GEN6_VE0_VALID |
		  SURFACEFORMAT_R32G32_FLOAT << VE0_FORMAT_SHIFT |
		  4 << VE0_OFFSET_SHIFT);	/* offset vb in bytes */
	OUT_BATCH(GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT |
		  GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT |
		  GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT |
		  GEN4_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_3_SHIFT);
}

/*
 * gen7_emit_vertex_buffer emit the vertex buffers command
 *
 * @batch
 * @offset - bytw offset within the @batch where the vertex buffer starts.
 */
static void gen7_emit_vertex_buffer(struct intel_batchbuffer *batch,
				    uint32_t offset) {
	OUT_BATCH(GEN4_3DSTATE_VERTEX_BUFFERS | (1 + (4 * 1) - 2));
	OUT_BATCH(0 << GEN6_VB0_BUFFER_INDEX_SHIFT | /* VB 0th index */
		  GEN8_VB0_BUFFER_ADDR_MOD_EN | /* Address Modify Enable */
		  VERTEX_SIZE << VB0_BUFFER_PITCH_SHIFT);
	OUT_RELOC(batch->bo, I915_GEM_DOMAIN_VERTEX, 0, offset);
	OUT_BATCH(3 * VERTEX_SIZE);
}

static uint32_t
gen6_create_cc_state(struct intel_batchbuffer *batch)
{
	struct gen6_color_calc_state *cc_state;
	uint32_t offset;

	cc_state = intel_batchbuffer_subdata_alloc(batch,
						   sizeof(*cc_state), 64);
	offset = intel_batchbuffer_subdata_offset(batch, cc_state);
	annotation_add_state(&aub_annotations, AUB_TRACE_CC_STATE,
			     offset, sizeof(*cc_state));

	return offset;
}

static uint32_t
gen8_create_blend_state(struct intel_batchbuffer *batch)
{
	struct gen8_blend_state *blend;
	int i;
	uint32_t offset;

	blend = intel_batchbuffer_subdata_alloc(batch, sizeof(*blend), 64);
	offset = intel_batchbuffer_subdata_offset(batch, blend);
	annotation_add_state(&aub_annotations, AUB_TRACE_BLEND_STATE,
			     offset, sizeof(*blend));

	for (i = 0; i < 16; i++) {
		blend->bs[i].dest_blend_factor = GEN6_BLENDFACTOR_ZERO;
		blend->bs[i].source_blend_factor = GEN6_BLENDFACTOR_ONE;
		blend->bs[i].color_blend_func = GEN6_BLENDFUNCTION_ADD;
		blend->bs[i].pre_blend_color_clamp = 1;
		blend->bs[i].color_buffer_blend = 0;
	}

	return offset;
}

static uint32_t
gen6_create_cc_viewport(struct intel_batchbuffer *batch)
{
	struct gen4_cc_viewport *vp;
	uint32_t offset;

	vp = intel_batchbuffer_subdata_alloc(batch, sizeof(*vp), 32);
	offset = intel_batchbuffer_subdata_offset(batch, vp);
	annotation_add_state(&aub_annotations, AUB_TRACE_CC_VP_STATE,
			     offset, sizeof(*vp));

	/* XXX I don't understand this */
	vp->min_depth = -1.e35;
	vp->max_depth = 1.e35;

	return offset;
}

static uint32_t
gen7_create_sf_clip_viewport(struct intel_batchbuffer *batch) {
	/* XXX these are likely not needed */
	struct gen7_sf_clip_viewport *scv_state;
	uint32_t offset;

	scv_state = intel_batchbuffer_subdata_alloc(batch,
						    sizeof(*scv_state), 64);
	offset = intel_batchbuffer_subdata_offset(batch, scv_state);
	annotation_add_state(&aub_annotations, AUB_TRACE_CLIP_VP_STATE,
			     offset, sizeof(*scv_state));

	scv_state->guardband.xmin = 0;
	scv_state->guardband.xmax = 1.0f;
	scv_state->guardband.ymin = 0;
	scv_state->guardband.ymax = 1.0f;

	return offset;
}

static uint32_t
gen6_create_scissor_rect(struct intel_batchbuffer *batch)
{
	struct gen6_scissor_rect *scissor;
	uint32_t offset;

	scissor = intel_batchbuffer_subdata_alloc(batch, sizeof(*scissor), 64);
	offset = intel_batchbuffer_subdata_offset(batch, scissor);
	annotation_add_state(&aub_annotations, AUB_TRACE_SCISSOR_STATE,
			     offset, sizeof(*scissor));

	return offset;
}

static void
gen8_emit_sip(struct intel_batchbuffer *batch) {
	OUT_BATCH(GEN4_STATE_SIP | (3 - 2));
	OUT_BATCH(0);
	OUT_BATCH(0);
}

static void
gen7_emit_push_constants(struct intel_batchbuffer *batch) {
	OUT_BATCH(GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_VS);
	OUT_BATCH(0);
	OUT_BATCH(GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_HS);
	OUT_BATCH(0);
	OUT_BATCH(GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_DS);
	OUT_BATCH(0);
	OUT_BATCH(GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_GS);
	OUT_BATCH(0);
	OUT_BATCH(GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_PS);
	OUT_BATCH(0);
}

static void
gen9_emit_state_base_address(struct intel_batchbuffer *batch) {

	/* WaBindlessSurfaceStateModifyEnable:skl,bxt */
	/* The length has to be one less if we dont modify
	   bindless state */
	OUT_BATCH(GEN4_STATE_BASE_ADDRESS | (19 - 1 - 2));

	/* general */
	OUT_BATCH(0 | BASE_ADDRESS_MODIFY);
	OUT_BATCH(0);

	/* stateless data port */
	OUT_BATCH(0 | BASE_ADDRESS_MODIFY);

	/* surface */
	OUT_RELOC(batch->bo, I915_GEM_DOMAIN_SAMPLER, 0, BASE_ADDRESS_MODIFY);

	/* dynamic */
	OUT_RELOC(batch->bo, I915_GEM_DOMAIN_RENDER | I915_GEM_DOMAIN_INSTRUCTION,
		  0, BASE_ADDRESS_MODIFY);

	/* indirect */
	OUT_BATCH(0);
	OUT_BATCH(0);

	/* instruction */
	OUT_RELOC(batch->bo, I915_GEM_DOMAIN_INSTRUCTION, 0, BASE_ADDRESS_MODIFY);

	/* general state buffer size */
	OUT_BATCH(0xfffff000 | 1);
	/* dynamic state buffer size */
	OUT_BATCH(1 << 12 | 1);
	/* indirect object buffer size */
	OUT_BATCH(0xfffff000 | 1);
	/* intruction buffer size */
	OUT_BATCH(1 << 12 | 1);

	/* Bindless surface state base address */
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
}

static void
gen7_emit_urb(struct intel_batchbuffer *batch) {
	/* XXX: Min valid values from mesa */
	const int vs_entries = 64;
	const int vs_size = 2;
	const int vs_start = 4;

	OUT_BATCH(GEN7_3DSTATE_URB_VS);
	OUT_BATCH(vs_entries | ((vs_size - 1) << 16) | (vs_start << 25));
	OUT_BATCH(GEN7_3DSTATE_URB_GS);
	OUT_BATCH(vs_start << 25);
	OUT_BATCH(GEN7_3DSTATE_URB_HS);
	OUT_BATCH(vs_start << 25);
	OUT_BATCH(GEN7_3DSTATE_URB_DS);
	OUT_BATCH(vs_start << 25);
}

static void
gen8_emit_cc(struct intel_batchbuffer *batch) {
	OUT_BATCH(GEN7_3DSTATE_BLEND_STATE_POINTERS);
	OUT_BATCH(cc.blend_state | 1);

	OUT_BATCH(GEN6_3DSTATE_CC_STATE_POINTERS);
	OUT_BATCH(cc.cc_state | 1);
}

static void
gen8_emit_multisample(struct intel_batchbuffer *batch) {
	OUT_BATCH(GEN8_3DSTATE_MULTISAMPLE | 0);
	OUT_BATCH(0);

	OUT_BATCH(GEN6_3DSTATE_SAMPLE_MASK);
	OUT_BATCH(1);
}

static void
gen8_emit_vs(struct intel_batchbuffer *batch) {
	OUT_BATCH(GEN6_3DSTATE_CONSTANT_VS | (11-2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_BINDING_TABLE_POINTERS_VS);
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_SAMPLER_STATE_POINTERS_VS);
	OUT_BATCH(0);

	OUT_BATCH(GEN6_3DSTATE_VS | (9-2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
}

static void
gen8_emit_hs(struct intel_batchbuffer *batch) {
	OUT_BATCH(GEN7_3DSTATE_CONSTANT_HS | (11-2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_HS | (9-2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_BINDING_TABLE_POINTERS_HS);
	OUT_BATCH(0);

	OUT_BATCH(GEN8_3DSTATE_SAMPLER_STATE_POINTERS_HS);
	OUT_BATCH(0);
}

static void
gen8_emit_gs(struct intel_batchbuffer *batch) {
	OUT_BATCH(GEN6_3DSTATE_CONSTANT_GS | (11-2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

	OUT_BATCH(GEN6_3DSTATE_GS | (10-2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_BINDING_TABLE_POINTERS_GS);
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_SAMPLER_STATE_POINTERS_GS);
	OUT_BATCH(0);
}

static void
gen9_emit_ds(struct intel_batchbuffer *batch) {
	OUT_BATCH(GEN7_3DSTATE_CONSTANT_DS | (11-2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_DS | (11-2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_BINDING_TABLE_POINTERS_DS);
	OUT_BATCH(0);

	OUT_BATCH(GEN8_3DSTATE_SAMPLER_STATE_POINTERS_DS);
	OUT_BATCH(0);
}


static void
gen8_emit_wm_hz_op(struct intel_batchbuffer *batch) {
	OUT_BATCH(GEN8_3DSTATE_WM_HZ_OP | (5-2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
}

static void
gen8_emit_null_state(struct intel_batchbuffer *batch) {
	gen8_emit_wm_hz_op(batch);
	gen8_emit_hs(batch);
	OUT_BATCH(GEN7_3DSTATE_TE | (4-2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	gen8_emit_gs(batch);
	gen9_emit_ds(batch);
	gen8_emit_vs(batch);
}

static void
gen7_emit_clip(struct intel_batchbuffer *batch) {
	OUT_BATCH(GEN6_3DSTATE_CLIP | (4 - 2));
	OUT_BATCH(0);
	OUT_BATCH(0); /*  pass-through */
	OUT_BATCH(0);
}

static void
gen8_emit_sf(struct intel_batchbuffer *batch)
{
	int i;

	OUT_BATCH(GEN7_3DSTATE_SBE | (6 - 2));
	OUT_BATCH(1 << GEN7_SBE_NUM_OUTPUTS_SHIFT |
		  GEN8_SBE_FORCE_URB_ENTRY_READ_LENGTH |
		  GEN8_SBE_FORCE_URB_ENTRY_READ_OFFSET |
		  1 << GEN7_SBE_URB_ENTRY_READ_LENGTH_SHIFT |
		  1 << GEN8_SBE_URB_ENTRY_READ_OFFSET_SHIFT);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(GEN9_SBE_ACTIVE_COMPONENT_XYZW << 0);
	OUT_BATCH(0);

	OUT_BATCH(GEN8_3DSTATE_SBE_SWIZ | (11 - 2));
	for (i = 0; i < 8; i++)
		OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

	OUT_BATCH(GEN8_3DSTATE_RASTER | (5 - 2));
	OUT_BATCH(GEN8_RASTER_FRONT_WINDING_CCW | GEN8_RASTER_CULL_NONE);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

	OUT_BATCH(GEN6_3DSTATE_SF | (4 - 2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
}

static void
gen8_emit_ps(struct intel_batchbuffer *batch, uint32_t kernel) {
	const int max_threads = 63;

	OUT_BATCH(GEN6_3DSTATE_WM | (2 - 2));
	OUT_BATCH(/* XXX: I don't understand the BARYCENTRIC stuff, but it
		   * appears we need it to put our setup data in the place we
		   * expect (g6, see below) */
		  GEN8_3DSTATE_PS_PERSPECTIVE_PIXEL_BARYCENTRIC);

	OUT_BATCH(GEN6_3DSTATE_CONSTANT_PS | (11-2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_PS | (12-2));
	OUT_BATCH(kernel);
	OUT_BATCH(0); /* kernel hi */
	OUT_BATCH(1 << GEN6_3DSTATE_WM_SAMPLER_COUNT_SHIFT |
		  2 << GEN6_3DSTATE_WM_BINDING_TABLE_ENTRY_COUNT_SHIFT);
	OUT_BATCH(0); /* scratch space stuff */
	OUT_BATCH(0); /* scratch hi */
	OUT_BATCH((max_threads - 1) << GEN8_3DSTATE_PS_MAX_THREADS_SHIFT |
		  GEN6_3DSTATE_WM_16_DISPATCH_ENABLE);
	OUT_BATCH(6 << GEN6_3DSTATE_WM_DISPATCH_START_GRF_0_SHIFT);
	OUT_BATCH(0); // kernel 1
	OUT_BATCH(0); /* kernel 1 hi */
	OUT_BATCH(0); // kernel 2
	OUT_BATCH(0); /* kernel 2 hi */

	OUT_BATCH(GEN8_3DSTATE_PS_BLEND | (2 - 2));
	OUT_BATCH(GEN8_PS_BLEND_HAS_WRITEABLE_RT);

	OUT_BATCH(GEN8_3DSTATE_PS_EXTRA | (2 - 2));
	OUT_BATCH(GEN8_PSX_PIXEL_SHADER_VALID | GEN8_PSX_ATTRIBUTE_ENABLE);
}

static void
gen9_emit_depth(struct intel_batchbuffer *batch)
{
	OUT_BATCH(GEN8_3DSTATE_WM_DEPTH_STENCIL | (4 - 2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

	OUT_BATCH(GEN7_3DSTATE_DEPTH_BUFFER | (8-2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

	OUT_BATCH(GEN8_3DSTATE_HIER_DEPTH_BUFFER | (5-2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

	OUT_BATCH(GEN8_3DSTATE_STENCIL_BUFFER | (5-2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
}

static void
gen7_emit_clear(struct intel_batchbuffer *batch) {
	OUT_BATCH(GEN7_3DSTATE_CLEAR_PARAMS | (3-2));
	OUT_BATCH(0);
	OUT_BATCH(1); // clear valid
}

static void
gen6_emit_drawing_rectangle(struct intel_batchbuffer *batch, const struct igt_buf *dst)
{
	OUT_BATCH(GEN4_3DSTATE_DRAWING_RECTANGLE | (4 - 2));
	OUT_BATCH(0);
	OUT_BATCH((igt_buf_height(dst) - 1) << 16 | (igt_buf_width(dst) - 1));
	OUT_BATCH(0);
}

static void gen8_emit_vf_topology(struct intel_batchbuffer *batch)
{
	OUT_BATCH(GEN8_3DSTATE_VF_TOPOLOGY);
	OUT_BATCH(_3DPRIM_RECTLIST);
}

/* Vertex elements MUST be defined before this according to spec */
static void gen8_emit_primitive(struct intel_batchbuffer *batch, uint32_t offset)
{
	OUT_BATCH(GEN8_3DSTATE_VF | (2 - 2));
	OUT_BATCH(0);

	OUT_BATCH(GEN8_3DSTATE_VF_INSTANCING | (3 - 2));
	OUT_BATCH(0);
	OUT_BATCH(0);

	OUT_BATCH(GEN4_3DPRIMITIVE | (7-2));
	OUT_BATCH(0);	/* gen8+ ignore the topology type field */
	OUT_BATCH(3);	/* vertex count */
	OUT_BATCH(0);	/*  We're specifying this instead with offset in GEN6_3DSTATE_VERTEX_BUFFERS */
	OUT_BATCH(1);	/* single instance */
	OUT_BATCH(0);	/* start instance location */
	OUT_BATCH(0);	/* index buffer offset, ignored */
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

static void
aux_pgtable_find_max_free_range(const struct igt_buf **bufs, int buf_count,
				uint64_t *range_start, uint64_t *range_size)
{
	/*
	 * Keep the first page reserved, so we can differentiate pinned
	 * objects based on a non-NULL offset.
	 */
	uint64_t start = 0x1000;
	/* For now alloc only from the first 4GB address space. */
	const uint64_t end = 1ULL << 32;
	uint64_t max_range_start = 0;
	uint64_t max_range_size = 0;
	int i;

	for (i = 0; i < buf_count; i++) {
		if (bufs[i]->bo->offset64 >= end)
			break;

		if (bufs[i]->bo->offset64 - start > max_range_size) {
			max_range_start = start;
			max_range_size = bufs[i]->bo->offset64 - start;
		}
		start = bufs[i]->bo->offset64 + bufs[i]->bo->size;
	}

	if (start < end && end - start > max_range_size) {
		max_range_start = start;
		max_range_size = end - start;
	}

	*range_start = max_range_start;
	*range_size = max_range_size;
}

static uint64_t
aux_pgtable_find_free_range(const struct igt_buf **bufs, int buf_count,
			    uint32_t size)
{
	uint64_t range_start;
	uint64_t range_size;
	/* A compressed surface must be 64kB aligned. */
	const uint32_t align = 0x10000;
	int pad;

	aux_pgtable_find_max_free_range(bufs, buf_count,
					&range_start, &range_size);

	pad = ALIGN(range_start, align) - range_start;
	range_start += pad;
	range_size -= pad;
	igt_assert(range_size >= size);

	return range_start +
	       ALIGN_DOWN(rand() % ((range_size - size) + 1), align);
}

static void
aux_pgtable_reserve_range(const struct igt_buf **bufs, int buf_count,
			  const struct igt_buf *new_buf)
{
	int i;

	if (new_buf->aux.stride) {
		uint64_t pin_offset = new_buf->bo->offset64;

		if (!pin_offset)
			pin_offset = aux_pgtable_find_free_range(bufs,
								 buf_count,
								 new_buf->bo->size);
		drm_intel_bo_set_softpin_offset(new_buf->bo, pin_offset);
		igt_assert(new_buf->bo->offset64 == pin_offset);
	}

	for (i = 0; i < buf_count; i++)
		if (bufs[i]->bo->offset64 > new_buf->bo->offset64)
			break;

	memmove(&bufs[i + 1], &bufs[i], sizeof(bufs[0]) * (buf_count - i));

	bufs[i] = new_buf;
}

struct aux_pgtable_info {
	int buf_count;
	const struct igt_buf *bufs[2];
	uint64_t buf_pin_offsets[2];
	drm_intel_bo *pgtable_bo;
};

static void
gen12_aux_pgtable_init(struct aux_pgtable_info *info,
		       drm_intel_bufmgr *bufmgr,
		       const struct igt_buf *src_buf,
		       const struct igt_buf *dst_buf)
{
	const struct igt_buf *bufs[2];
	const struct igt_buf *reserved_bufs[2];
	int reserved_buf_count;
	int i;

	if (!src_buf->aux.stride && !dst_buf->aux.stride)
		return;

	bufs[0] = src_buf;
	bufs[1] = dst_buf;

	/*
	 * Ideally we'd need an IGT-wide GFX address space allocator, which
	 * would consider all allocations and thus avoid evictions. For now use
	 * a simpler scheme here, which only considers the buffers involved in
	 * the blit, which should at least minimize the chance for evictions
	 * in the case of subsequent blits:
	 *   1. If they were already bound (bo->offset64 != 0), use this
	 *      address.
	 *   2. Pick a range randomly from the 4GB address space, that is not
	 *      already occupied by a bound object, or an object we pinned.
	 */
	reserved_buf_count = 0;
	/* First reserve space for any bufs that are bound already. */
	for (i = 0; i < ARRAY_SIZE(bufs); i++)
		if (bufs[i]->bo->offset64)
			aux_pgtable_reserve_range(reserved_bufs,
						  reserved_buf_count++,
						  bufs[i]);

	/* Next, reserve space for unbound bufs with an AUX surface. */
	for (i = 0; i < ARRAY_SIZE(bufs); i++)
		if (!bufs[i]->bo->offset64 && bufs[i]->aux.stride)
			aux_pgtable_reserve_range(reserved_bufs,
						  reserved_buf_count++,
						  bufs[i]);

	/* Create AUX pgtable entries only for bufs with an AUX surface */
	info->buf_count = 0;
	for (i = 0; i < reserved_buf_count; i++) {
		if (!reserved_bufs[i]->aux.stride)
			continue;

		info->bufs[info->buf_count] = reserved_bufs[i];
		info->buf_pin_offsets[info->buf_count] =
			reserved_bufs[i]->bo->offset64;
		info->buf_count++;
	}

	info->pgtable_bo = intel_aux_pgtable_create(bufmgr,
						    info->bufs,
						    info->buf_count);
	igt_assert(info->pgtable_bo);
}

static void
gen12_aux_pgtable_cleanup(struct aux_pgtable_info *info)
{
	int i;

	/* Check that the pinned bufs kept their offset after the exec. */
	for (i = 0; i < info->buf_count; i++)
		igt_assert_eq_u64(info->bufs[i]->bo->offset64,
				  info->buf_pin_offsets[i]);

	drm_intel_bo_unreference(info->pgtable_bo);
}

static uint32_t
gen12_create_aux_pgtable_state(struct intel_batchbuffer *batch,
			       drm_intel_bo *aux_pgtable_bo)
{
	uint64_t *pgtable_ptr;
	uint32_t pgtable_ptr_offset;
	int ret;

	if (!aux_pgtable_bo)
		return 0;

	pgtable_ptr = intel_batchbuffer_subdata_alloc(batch,
						      sizeof(*pgtable_ptr),
						      sizeof(*pgtable_ptr));
	pgtable_ptr_offset = intel_batchbuffer_subdata_offset(batch,
							      pgtable_ptr);

	*pgtable_ptr = aux_pgtable_bo->offset64;
	ret = drm_intel_bo_emit_reloc(batch->bo, pgtable_ptr_offset,
				      aux_pgtable_bo, 0,
				      0, 0);
	assert(ret == 0);

	return pgtable_ptr_offset;
}

static void
gen12_emit_aux_pgtable_state(struct intel_batchbuffer *batch, uint32_t state)
{
	if (!state)
		return;

	OUT_BATCH(MI_LOAD_REGISTER_MEM_GEN8);
	OUT_BATCH(GEN12_GFX_AUX_TABLE_BASE_ADDR);
	OUT_RELOC(batch->bo, 0, 0, state);

	OUT_BATCH(MI_LOAD_REGISTER_MEM_GEN8);
	OUT_BATCH(GEN12_GFX_AUX_TABLE_BASE_ADDR + 4);
	OUT_RELOC(batch->bo, 0, 0, state + 4);
}

static
void _gen9_render_copyfunc(struct intel_batchbuffer *batch,
			  drm_intel_context *context,
			  const struct igt_buf *src, unsigned src_x,
			  unsigned src_y, unsigned width, unsigned height,
			  const struct igt_buf *dst, unsigned dst_x,
			  unsigned dst_y,
			  drm_intel_bo *aux_pgtable_bo,
			  const uint32_t ps_kernel[][4],
			  uint32_t ps_kernel_size)
{
	uint32_t ps_sampler_state, ps_kernel_off, ps_binding_table;
	uint32_t scissor_state;
	uint32_t vertex_buffer;
	uint32_t batch_end;
	uint32_t aux_pgtable_state;

	igt_assert(src->bpp == dst->bpp);
	intel_batchbuffer_flush_with_context(batch, context);

	intel_batchbuffer_align(batch, 8);

	batch->ptr = &batch->buffer[BATCH_STATE_SPLIT];

	annotation_init(&aub_annotations);

	ps_binding_table  = gen8_bind_surfaces(batch, src, dst);
	ps_sampler_state  = gen8_create_sampler(batch);
	ps_kernel_off = gen8_fill_ps(batch, ps_kernel, ps_kernel_size);
	vertex_buffer = gen7_fill_vertex_buffer_data(batch, src,
						     src_x, src_y,
						     dst_x, dst_y,
						     width, height);
	cc.cc_state = gen6_create_cc_state(batch);
	cc.blend_state = gen8_create_blend_state(batch);
	viewport.cc_state = gen6_create_cc_viewport(batch);
	viewport.sf_clip_state = gen7_create_sf_clip_viewport(batch);
	scissor_state = gen6_create_scissor_rect(batch);

	aux_pgtable_state = gen12_create_aux_pgtable_state(batch,
							   aux_pgtable_bo);

	/* TODO: theree is other state which isn't setup */

	assert(batch->ptr < &batch->buffer[4095]);

	batch->ptr = batch->buffer;

	/* Start emitting the commands. The order roughly follows the mesa blorp
	 * order */
	OUT_BATCH(G4X_PIPELINE_SELECT | PIPELINE_SELECT_3D |
				GEN9_PIPELINE_SELECTION_MASK);

	gen12_emit_aux_pgtable_state(batch, aux_pgtable_state);

	gen8_emit_sip(batch);

	gen7_emit_push_constants(batch);

	gen9_emit_state_base_address(batch);

	OUT_BATCH(GEN7_3DSTATE_VIEWPORT_STATE_POINTERS_CC);
	OUT_BATCH(viewport.cc_state);
	OUT_BATCH(GEN8_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP);
	OUT_BATCH(viewport.sf_clip_state);

	gen7_emit_urb(batch);

	gen8_emit_cc(batch);

	gen8_emit_multisample(batch);

	gen8_emit_null_state(batch);

	OUT_BATCH(GEN7_3DSTATE_STREAMOUT | (5 - 2));
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);
	OUT_BATCH(0);

	gen7_emit_clip(batch);

	gen8_emit_sf(batch);

	gen8_emit_ps(batch, ps_kernel_off);

	OUT_BATCH(GEN7_3DSTATE_BINDING_TABLE_POINTERS_PS);
	OUT_BATCH(ps_binding_table);

	OUT_BATCH(GEN7_3DSTATE_SAMPLER_STATE_POINTERS_PS);
	OUT_BATCH(ps_sampler_state);

	OUT_BATCH(GEN8_3DSTATE_SCISSOR_STATE_POINTERS);
	OUT_BATCH(scissor_state);

	gen9_emit_depth(batch);

	gen7_emit_clear(batch);

	gen6_emit_drawing_rectangle(batch, dst);

	gen7_emit_vertex_buffer(batch, vertex_buffer);
	gen6_emit_vertex_elements(batch);

	gen8_emit_vf_topology(batch);
	gen8_emit_primitive(batch, vertex_buffer);

	OUT_BATCH(MI_BATCH_BUFFER_END);

	batch_end = intel_batchbuffer_align(batch, 8);
	assert(batch_end < BATCH_STATE_SPLIT);
	annotation_add_batch(&aub_annotations, batch_end);

	dump_batch(batch);

	annotation_flush(&aub_annotations, batch);

	gen6_render_flush(batch, context, batch_end);
	intel_batchbuffer_reset(batch);
}

void gen9_render_copyfunc(struct intel_batchbuffer *batch,
			  drm_intel_context *context,
			  const struct igt_buf *src, unsigned src_x, unsigned src_y,
			  unsigned width, unsigned height,
			  const struct igt_buf *dst, unsigned dst_x, unsigned dst_y)

{
	_gen9_render_copyfunc(batch, context, src, src_x, src_y,
			  width, height, dst, dst_x, dst_y, NULL,
			  ps_kernel_gen9, sizeof(ps_kernel_gen9));
}

void gen11_render_copyfunc(struct intel_batchbuffer *batch,
			  drm_intel_context *context,
			  const struct igt_buf *src, unsigned src_x, unsigned src_y,
			  unsigned width, unsigned height,
			  const struct igt_buf *dst, unsigned dst_x, unsigned dst_y)

{
	_gen9_render_copyfunc(batch, context, src, src_x, src_y,
			  width, height, dst, dst_x, dst_y, NULL,
			  ps_kernel_gen11, sizeof(ps_kernel_gen11));
}

void gen12_render_copyfunc(struct intel_batchbuffer *batch,
			   drm_intel_context *context,
			   const struct igt_buf *src, unsigned src_x, unsigned src_y,
			   unsigned width, unsigned height,
			   const struct igt_buf *dst, unsigned dst_x, unsigned dst_y)

{
	struct aux_pgtable_info pgtable_info = { };

	gen12_aux_pgtable_init(&pgtable_info, batch->bufmgr, src, dst);

	_gen9_render_copyfunc(batch, context, src, src_x, src_y,
			  width, height, dst, dst_x, dst_y,
			  pgtable_info.pgtable_bo,
			  gen12_render_copy,
			  sizeof(gen12_render_copy));

	gen12_aux_pgtable_cleanup(&pgtable_info);
}
