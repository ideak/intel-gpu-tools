#include "rendercopy.h"
#include "intel_chipset.h"
#include "gen4_render.h"
#include "surfaceformat.h"
#include "intel_bufops.h"

#include <assert.h>

#define VERTEX_SIZE (3*4)

#define URB_VS_ENTRY_SIZE	1
#define URB_GS_ENTRY_SIZE	0
#define URB_CL_ENTRY_SIZE	0
#define URB_SF_ENTRY_SIZE	2
#define URB_CS_ENTRY_SIZE	1

#define GEN4_GRF_BLOCKS(nreg) (((nreg) + 15) / 16 - 1)
#define SF_KERNEL_NUM_GRF 16
#define PS_KERNEL_NUM_GRF 32

static const uint32_t gen4_sf_kernel_nomask[][4] = {
	{ 0x00400031, 0x20c01fbd, 0x0069002c, 0x01110001 },
	{ 0x00600001, 0x206003be, 0x00690060, 0x00000000 },
	{ 0x00600040, 0x20e077bd, 0x00690080, 0x006940a0 },
	{ 0x00600041, 0x202077be, 0x008d00e0, 0x000000c0 },
	{ 0x00600040, 0x20e077bd, 0x006900a0, 0x00694060 },
	{ 0x00600041, 0x204077be, 0x008d00e0, 0x000000c8 },
	{ 0x00600031, 0x20001fbc, 0x008d0000, 0x8640c800 },
};

static const uint32_t gen5_sf_kernel_nomask[][4] = {
	{ 0x00400031, 0x20c01fbd, 0x1069002c, 0x02100001 },
	{ 0x00600001, 0x206003be, 0x00690060, 0x00000000 },
	{ 0x00600040, 0x20e077bd, 0x00690080, 0x006940a0 },
	{ 0x00600041, 0x202077be, 0x008d00e0, 0x000000c0 },
	{ 0x00600040, 0x20e077bd, 0x006900a0, 0x00694060 },
	{ 0x00600041, 0x204077be, 0x008d00e0, 0x000000c8 },
	{ 0x00600031, 0x20001fbc, 0x648d0000, 0x8808c800 },
};

static const uint32_t gen4_ps_kernel_nomask_affine[][4] = {
	{ 0x00800040, 0x23c06d29, 0x00480028, 0x10101010 },
	{ 0x00800040, 0x23806d29, 0x0048002a, 0x11001100 },
	{ 0x00802040, 0x2100753d, 0x008d03c0, 0x00004020 },
	{ 0x00802040, 0x2140753d, 0x008d0380, 0x00004024 },
	{ 0x00802059, 0x200077bc, 0x00000060, 0x008d0100 },
	{ 0x00802048, 0x204077be, 0x00000064, 0x008d0140 },
	{ 0x00802059, 0x200077bc, 0x00000070, 0x008d0100 },
	{ 0x00802048, 0x208077be, 0x00000074, 0x008d0140 },
	{ 0x00600201, 0x20200022, 0x008d0000, 0x00000000 },
	{ 0x00000201, 0x20280062, 0x00000000, 0x00000000 },
	{ 0x01800031, 0x21801d09, 0x008d0000, 0x02580001 },
	{ 0x00600001, 0x204003be, 0x008d0180, 0x00000000 },
	{ 0x00601001, 0x20c003be, 0x008d01a0, 0x00000000 },
	{ 0x00600001, 0x206003be, 0x008d01c0, 0x00000000 },
	{ 0x00601001, 0x20e003be, 0x008d01e0, 0x00000000 },
	{ 0x00600001, 0x208003be, 0x008d0200, 0x00000000 },
	{ 0x00601001, 0x210003be, 0x008d0220, 0x00000000 },
	{ 0x00600001, 0x20a003be, 0x008d0240, 0x00000000 },
	{ 0x00601001, 0x212003be, 0x008d0260, 0x00000000 },
	{ 0x00600201, 0x202003be, 0x008d0020, 0x00000000 },
	{ 0x00800031, 0x20001d28, 0x008d0000, 0x85a04800 },
};

static const uint32_t gen5_ps_kernel_nomask_affine[][4] = {
	{ 0x00800040, 0x23c06d29, 0x00480028, 0x10101010 },
	{ 0x00800040, 0x23806d29, 0x0048002a, 0x11001100 },
	{ 0x00802040, 0x2100753d, 0x008d03c0, 0x00004020 },
	{ 0x00802040, 0x2140753d, 0x008d0380, 0x00004024 },
	{ 0x00802059, 0x200077bc, 0x00000060, 0x008d0100 },
	{ 0x00802048, 0x204077be, 0x00000064, 0x008d0140 },
	{ 0x00802059, 0x200077bc, 0x00000070, 0x008d0100 },
	{ 0x00802048, 0x208077be, 0x00000074, 0x008d0140 },
	{ 0x01800031, 0x21801fa9, 0x208d0000, 0x0a8a0001 },
	{ 0x00802001, 0x304003be, 0x008d0180, 0x00000000 },
	{ 0x00802001, 0x306003be, 0x008d01c0, 0x00000000 },
	{ 0x00802001, 0x308003be, 0x008d0200, 0x00000000 },
	{ 0x00802001, 0x30a003be, 0x008d0240, 0x00000000 },
	{ 0x00600201, 0x202003be, 0x008d0020, 0x00000000 },
	{ 0x00800031, 0x20001d28, 0x548d0000, 0x94084800 },
};

static uint32_t
batch_round_upto(struct intel_bb *ibb, uint32_t divisor)
{
	uint32_t offset = intel_bb_offset(ibb);

	offset = (offset + divisor - 1) / divisor * divisor;
	intel_bb_ptr_set(ibb, offset);

	return offset;
}

static int gen4_max_vs_nr_urb_entries(uint32_t devid)
{
	return IS_GEN5(devid) ? 256 : 32;
}

static int gen4_max_sf_nr_urb_entries(uint32_t devid)
{
	return IS_GEN5(devid) ? 128 : 64;
}

static int gen4_urb_size(uint32_t devid)
{
	return IS_GEN5(devid) ? 1024 : IS_G4X(devid) ? 384 : 256;
}

static int gen4_max_sf_threads(uint32_t devid)
{
	return IS_GEN5(devid) ? 48 : 24;
}

static int gen4_max_wm_threads(uint32_t devid)
{
	return IS_GEN5(devid) ? 72 : IS_G4X(devid) ? 50 : 32;
}

static uint32_t
gen4_bind_buf(struct intel_bb *ibb, const struct intel_buf *buf, int is_dst)
{
	struct gen4_surface_state *ss;
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

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*ss));
}

static uint32_t
gen4_bind_surfaces(struct intel_bb *ibb,
		   const struct intel_buf *src,
		   const struct intel_buf *dst)
{
	uint32_t *binding_table, binding_table_offset;

	binding_table = intel_bb_ptr_align(ibb, 32);
	binding_table_offset = intel_bb_ptr_add_return_prev_offset(ibb, 32);

	binding_table[0] = gen4_bind_buf(ibb, dst, 1);
	binding_table[1] = gen4_bind_buf(ibb, src, 0);

	return binding_table_offset;
}

static void
gen4_emit_sip(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN4_STATE_SIP | (2 - 2));
	intel_bb_out(ibb, 0);
}

static void
gen4_emit_state_base_address(struct intel_bb *ibb)
{
	if (IS_GEN5(ibb->devid)) {
		intel_bb_out(ibb, GEN4_STATE_BASE_ADDRESS | (8 - 2));
		intel_bb_emit_reloc(ibb, ibb->handle, /* general */
				    I915_GEM_DOMAIN_INSTRUCTION, 0,
				    BASE_ADDRESS_MODIFY, ibb->batch_offset);
		intel_bb_emit_reloc(ibb, ibb->handle, /* surface */
				    I915_GEM_DOMAIN_INSTRUCTION, 0,
				    BASE_ADDRESS_MODIFY, ibb->batch_offset);
		intel_bb_out(ibb, 0); /* media */
		intel_bb_emit_reloc(ibb, ibb->handle, /* instruction */
				    I915_GEM_DOMAIN_INSTRUCTION, 0,
				    BASE_ADDRESS_MODIFY, ibb->batch_offset);

		/* upper bounds, disable */
		intel_bb_out(ibb, BASE_ADDRESS_MODIFY); /* general */
		intel_bb_out(ibb, 0); /* media */
		intel_bb_out(ibb, BASE_ADDRESS_MODIFY); /* instruction */
	} else {
		intel_bb_out(ibb, GEN4_STATE_BASE_ADDRESS | (6 - 2));
		intel_bb_emit_reloc(ibb, ibb->handle, /* general */
				    I915_GEM_DOMAIN_INSTRUCTION, 0,
				    BASE_ADDRESS_MODIFY, ibb->batch_offset);
		intel_bb_emit_reloc(ibb, ibb->handle, /* surface */
				    I915_GEM_DOMAIN_INSTRUCTION, 0,
				    BASE_ADDRESS_MODIFY, ibb->batch_offset);
		intel_bb_out(ibb, 0); /* media */

		/* upper bounds, disable */
		intel_bb_out(ibb, BASE_ADDRESS_MODIFY); /* general */
		intel_bb_out(ibb, 0); /* media */
	}
}

static void
gen4_emit_pipelined_pointers(struct intel_bb *ibb,
			     uint32_t vs, uint32_t sf,
			     uint32_t wm, uint32_t cc)
{
	intel_bb_out(ibb, GEN4_3DSTATE_PIPELINED_POINTERS | (7 - 2));
	intel_bb_out(ibb, vs);
	intel_bb_out(ibb, GEN4_GS_DISABLE);
	intel_bb_out(ibb, GEN4_CLIP_DISABLE);
	intel_bb_out(ibb, sf);
	intel_bb_out(ibb, wm);
	intel_bb_out(ibb, cc);
}

static void
gen4_emit_urb(struct intel_bb *ibb)
{
	int vs_entries = gen4_max_vs_nr_urb_entries(ibb->devid);
	int gs_entries = 0;
	int cl_entries = 0;
	int sf_entries = gen4_max_sf_nr_urb_entries(ibb->devid);
	int cs_entries = 0;

	int urb_vs_end =              vs_entries * URB_VS_ENTRY_SIZE;
	int urb_gs_end = urb_vs_end + gs_entries * URB_GS_ENTRY_SIZE;
	int urb_cl_end = urb_gs_end + cl_entries * URB_CL_ENTRY_SIZE;
	int urb_sf_end = urb_cl_end + sf_entries * URB_SF_ENTRY_SIZE;
	int urb_cs_end = urb_sf_end + cs_entries * URB_CS_ENTRY_SIZE;

	assert(urb_cs_end <= gen4_urb_size(ibb->devid));

	intel_bb_ptr_align(ibb, 16);

	intel_bb_out(ibb, GEN4_URB_FENCE |
		     UF0_CS_REALLOC |
		     UF0_SF_REALLOC |
		     UF0_CLIP_REALLOC |
		     UF0_GS_REALLOC |
		     UF0_VS_REALLOC |
		     (3 - 2));
	intel_bb_out(ibb, urb_cl_end << UF1_CLIP_FENCE_SHIFT |
		     urb_gs_end << UF1_GS_FENCE_SHIFT |
		     urb_vs_end << UF1_VS_FENCE_SHIFT);
	intel_bb_out(ibb, urb_cs_end << UF2_CS_FENCE_SHIFT |
		     urb_sf_end << UF2_SF_FENCE_SHIFT);

	intel_bb_out(ibb, GEN4_CS_URB_STATE | (2 - 2));
	intel_bb_out(ibb, (URB_CS_ENTRY_SIZE - 1) << 4 | cs_entries << 0);
}

static void
gen4_emit_null_depth_buffer(struct intel_bb *ibb)
{
	if (IS_G4X(ibb->devid) || IS_GEN5(ibb->devid)) {
		intel_bb_out(ibb, GEN4_3DSTATE_DEPTH_BUFFER | (6 - 2));
		intel_bb_out(ibb, SURFACE_NULL << GEN4_3DSTATE_DEPTH_BUFFER_TYPE_SHIFT |
			     GEN4_DEPTHFORMAT_D32_FLOAT << GEN4_3DSTATE_DEPTH_BUFFER_FORMAT_SHIFT);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);
	} else {
		intel_bb_out(ibb, GEN4_3DSTATE_DEPTH_BUFFER | (5 - 2));
		intel_bb_out(ibb, SURFACE_NULL << GEN4_3DSTATE_DEPTH_BUFFER_TYPE_SHIFT |
			     GEN4_DEPTHFORMAT_D32_FLOAT << GEN4_3DSTATE_DEPTH_BUFFER_FORMAT_SHIFT);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);
	}

	if (IS_GEN5(ibb->devid)) {
		intel_bb_out(ibb, GEN4_3DSTATE_CLEAR_PARAMS | (2 - 2));
		intel_bb_out(ibb, 0);
	}
}

static void
gen4_emit_invariant(struct intel_bb *ibb)
{
	intel_bb_out(ibb, MI_FLUSH | MI_INHIBIT_RENDER_CACHE_FLUSH);

	if (IS_GEN5(ibb->devid) || IS_G4X(ibb->devid))
		intel_bb_out(ibb, G4X_PIPELINE_SELECT | PIPELINE_SELECT_3D);
	else
		intel_bb_out(ibb, GEN4_PIPELINE_SELECT | PIPELINE_SELECT_3D);
}

static uint32_t
gen4_create_vs_state(struct intel_bb *ibb)
{
	struct gen4_vs_state *vs;
	int nr_urb_entries;

	vs = intel_bb_ptr_align(ibb, 32);

	/* Set up the vertex shader to be disabled (passthrough) */
	nr_urb_entries = gen4_max_vs_nr_urb_entries(ibb->devid);
	if (IS_GEN5(ibb->devid))
		nr_urb_entries >>= 2;
	vs->vs4.nr_urb_entries = nr_urb_entries;
	vs->vs4.urb_entry_allocation_size = URB_VS_ENTRY_SIZE - 1;
	vs->vs6.vs_enable = 0;
	vs->vs6.vert_cache_disable = 1;

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*vs));
}

static uint32_t
gen4_create_sf_state(struct intel_bb *ibb,
		     uint32_t kernel)
{
	struct gen4_sf_state *sf;

	sf = intel_bb_ptr_align(ibb, 32);

	sf->sf0.grf_reg_count = GEN4_GRF_BLOCKS(SF_KERNEL_NUM_GRF);
	sf->sf0.kernel_start_pointer = kernel >> 6;

	sf->sf3.urb_entry_read_length = 1;  /* 1 URB per vertex */
	/* don't smash vertex header, read start from dw8 */
	sf->sf3.urb_entry_read_offset = 1;
	sf->sf3.dispatch_grf_start_reg = 3;

	sf->sf4.max_threads = gen4_max_sf_threads(ibb->devid) - 1;
	sf->sf4.urb_entry_allocation_size = URB_SF_ENTRY_SIZE - 1;
	sf->sf4.nr_urb_entries = gen4_max_sf_nr_urb_entries(ibb->devid);

	sf->sf6.cull_mode = GEN4_CULLMODE_NONE;
	sf->sf6.dest_org_vbias = 0x8;
	sf->sf6.dest_org_hbias = 0x8;

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*sf));
}

static uint32_t
gen4_create_wm_state(struct intel_bb *ibb,
		     uint32_t kernel,
		     uint32_t sampler)
{
	struct gen4_wm_state *wm;

	wm = intel_bb_ptr_align(ibb, 32);

	assert((kernel & 63) == 0);
	wm->wm0.kernel_start_pointer = kernel >> 6;
	wm->wm0.grf_reg_count = GEN4_GRF_BLOCKS(PS_KERNEL_NUM_GRF);

	wm->wm3.urb_entry_read_offset = 0;
	wm->wm3.dispatch_grf_start_reg = 3;

	assert((sampler & 31) == 0);
	wm->wm4.sampler_state_pointer = sampler >> 5;
	wm->wm4.sampler_count = 1;

	wm->wm5.max_threads = gen4_max_wm_threads(ibb->devid);
	wm->wm5.thread_dispatch_enable = 1;
	wm->wm5.enable_16_pix = 1;
	wm->wm5.early_depth_test = 1;

	if (IS_GEN5(ibb->devid))
		wm->wm1.binding_table_entry_count = 0;
	else
		wm->wm1.binding_table_entry_count = 2;
	wm->wm3.urb_entry_read_length = 2;

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*wm));
}

static void
gen4_emit_binding_table(struct intel_bb *ibb,
			uint32_t wm_table)
{
	intel_bb_out(ibb, GEN4_3DSTATE_BINDING_TABLE_POINTERS | (6 - 2));
	intel_bb_out(ibb, 0);		/* vs */
	intel_bb_out(ibb, 0);		/* gs */
	intel_bb_out(ibb, 0);		/* clip */
	intel_bb_out(ibb, 0);		/* sf */
	intel_bb_out(ibb, wm_table);    /* ps */
}

static void
gen4_emit_drawing_rectangle(struct intel_bb *ibb,
			    const struct intel_buf *dst)
{
	intel_bb_out(ibb, GEN4_3DSTATE_DRAWING_RECTANGLE | (4 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, (intel_buf_height(dst) - 1) << 16 |
		     (intel_buf_width(dst) - 1));
	intel_bb_out(ibb, 0);
}

static void
gen4_emit_vertex_elements(struct intel_bb *ibb)
{

	if (IS_GEN5(ibb->devid)) {
		/* The VUE layout
		 *    dword 0-3: pad (0.0, 0.0, 0.0, 0.0),
		 *    dword 4-7: position (x, y, 1.0, 1.0),
		 *    dword 8-11: texture coordinate 0 (u0, v0, 0, 0)
		 *
		 * dword 4-11 are fetched from vertex buffer
		 */
		intel_bb_out(ibb, GEN4_3DSTATE_VERTEX_ELEMENTS | (3 * 2 + 1 - 2));

		/* pad */
		intel_bb_out(ibb, 0 << GEN4_VE0_VERTEX_BUFFER_INDEX_SHIFT | GEN4_VE0_VALID |
			     SURFACEFORMAT_R32G32B32A32_FLOAT << VE0_FORMAT_SHIFT |
			     0 << VE0_OFFSET_SHIFT);
		intel_bb_out(ibb, GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_0_SHIFT |
			     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_1_SHIFT |
			     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT |
			     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_3_SHIFT);

		/* x,y */
		intel_bb_out(ibb, 0 << GEN4_VE0_VERTEX_BUFFER_INDEX_SHIFT | GEN4_VE0_VALID |
			     SURFACEFORMAT_R16G16_SSCALED << VE0_FORMAT_SHIFT |
			     0 << VE0_OFFSET_SHIFT);
		intel_bb_out(ibb, GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT |
			     GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT |
			     GEN4_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_2_SHIFT |
			     GEN4_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_3_SHIFT);

		/* u0, v0 */
		intel_bb_out(ibb, 0 << GEN4_VE0_VERTEX_BUFFER_INDEX_SHIFT | GEN4_VE0_VALID |
			     SURFACEFORMAT_R32G32_FLOAT << VE0_FORMAT_SHIFT |
			     4 << VE0_OFFSET_SHIFT);
		intel_bb_out(ibb, GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT |
			     GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT |
			     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT |
			     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_3_SHIFT);
	} else {
		/* The VUE layout
		 *    dword 0-3: position (x, y, 1.0, 1.0),
		 *    dword 4-7: texture coordinate 0 (u0, v0, 0, 0)
		 *
		 * dword 0-7 are fetched from vertex buffer
		 */
		intel_bb_out(ibb, GEN4_3DSTATE_VERTEX_ELEMENTS | (2 * 2 + 1 - 2));

		/* x,y */
		intel_bb_out(ibb, 0 << GEN4_VE0_VERTEX_BUFFER_INDEX_SHIFT | GEN4_VE0_VALID |
			     SURFACEFORMAT_R16G16_SSCALED << VE0_FORMAT_SHIFT |
			     0 << VE0_OFFSET_SHIFT);
		intel_bb_out(ibb, GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT |
			     GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT |
			     GEN4_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_2_SHIFT |
			     GEN4_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_3_SHIFT |
			     4 << VE1_DESTINATION_ELEMENT_OFFSET_SHIFT);

		/* u0, v0 */
		intel_bb_out(ibb, 0 << GEN4_VE0_VERTEX_BUFFER_INDEX_SHIFT | GEN4_VE0_VALID |
			     SURFACEFORMAT_R32G32_FLOAT << VE0_FORMAT_SHIFT |
			     4 << VE0_OFFSET_SHIFT);
		intel_bb_out(ibb, GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT |
			     GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT |
			     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT |
			     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_3_SHIFT |
			     8 << VE1_DESTINATION_ELEMENT_OFFSET_SHIFT);
	}
}

static uint32_t
gen4_create_cc_viewport(struct intel_bb *ibb)
{
	struct gen4_cc_viewport *vp;

	vp = intel_bb_ptr_align(ibb, 32);

	vp->min_depth = -1.e35;
	vp->max_depth = 1.e35;

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*vp));
}

static uint32_t
gen4_create_cc_state(struct intel_bb *ibb,
		     uint32_t cc_vp)
{
	struct gen4_color_calc_state *cc;

	cc = intel_bb_ptr_align(ibb, 64);

	cc->cc4.cc_viewport_state_offset = cc_vp;

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*cc));
}

static uint32_t
gen4_create_sf_kernel(struct intel_bb *ibb)
{
	if (IS_GEN5(ibb->devid))
		return intel_bb_copy_data(ibb, gen5_sf_kernel_nomask,
					  sizeof(gen5_sf_kernel_nomask), 64);
	else
		return intel_bb_copy_data(ibb, gen4_sf_kernel_nomask,
					  sizeof(gen4_sf_kernel_nomask), 64);
}

static uint32_t
gen4_create_ps_kernel(struct intel_bb *ibb)
{
	if (IS_GEN5(ibb->devid))
		return intel_bb_copy_data(ibb, gen5_ps_kernel_nomask_affine,
					  sizeof(gen5_ps_kernel_nomask_affine),
					  64);
	else
		return intel_bb_copy_data(ibb, gen4_ps_kernel_nomask_affine,
					  sizeof(gen4_ps_kernel_nomask_affine),
					  64);
}

static uint32_t
gen4_create_sampler(struct intel_bb *ibb,
		    sampler_filter_t filter,
		    sampler_extend_t extend)
{
	struct gen4_sampler_state *ss;

	ss = intel_bb_ptr_align(ibb, 32);

	ss->ss0.lod_preclamp = GEN4_LOD_PRECLAMP_OGL;

	/* We use the legacy mode to get the semantics specified by
	 * the Render extension.
	 */
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

static void gen4_emit_vertex_buffer(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN4_3DSTATE_VERTEX_BUFFERS | (5 - 2));
	intel_bb_out(ibb, GEN4_VB0_VERTEXDATA |
		     0 << GEN4_VB0_BUFFER_INDEX_SHIFT |
		     VERTEX_SIZE << VB0_BUFFER_PITCH_SHIFT);
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_VERTEX, 0, 0, ibb->batch_offset);
	if (IS_GEN5(ibb->devid))
		intel_bb_emit_reloc(ibb, ibb->handle,
				    I915_GEM_DOMAIN_VERTEX, 0,
				    ibb->size - 1, ibb->batch_offset);
	else
		intel_bb_out(ibb, ibb->size / VERTEX_SIZE - 1);
	intel_bb_out(ibb, 0);
}

static uint32_t gen4_emit_primitive(struct intel_bb *ibb)
{
	uint32_t offset;

	intel_bb_out(ibb, GEN4_3DPRIMITIVE |
		     GEN4_3DPRIMITIVE_VERTEX_SEQUENTIAL |
		     _3DPRIM_RECTLIST << GEN4_3DPRIMITIVE_TOPOLOGY_SHIFT |
		     0 << 9 |
		     (6 - 2));
	intel_bb_out(ibb, 3);	/* vertex count */
	offset = intel_bb_offset(ibb);
	intel_bb_out(ibb, 0);	/* vertex_index */
	intel_bb_out(ibb, 1);	/* single instance */
	intel_bb_out(ibb, 0);	/* start instance location */
	intel_bb_out(ibb, 0);	/* index buffer offset, ignored */

	return offset;
}

void gen4_render_copyfunc(struct intel_bb *ibb,
			  struct intel_buf *src,
			  uint32_t src_x, uint32_t src_y,
			  uint32_t width, uint32_t height,
			  struct intel_buf *dst,
			  uint32_t dst_x, uint32_t dst_y)
{
	uint32_t cc, cc_vp;
	uint32_t wm, wm_sampler, wm_kernel, wm_table;
	uint32_t sf, sf_kernel;
	uint32_t vs;
	uint32_t offset, batch_end;

	igt_assert(src->bpp == dst->bpp);

	intel_bb_flush_render(ibb);

	intel_bb_add_intel_buf(ibb, dst, true);
	intel_bb_add_intel_buf(ibb, src, false);

	intel_bb_ptr_set(ibb, 1024 + 64);

	vs = gen4_create_vs_state(ibb);

	sf_kernel = gen4_create_sf_kernel(ibb);
	sf = gen4_create_sf_state(ibb, sf_kernel);

	wm_table = gen4_bind_surfaces(ibb, src, dst);
	wm_kernel = gen4_create_ps_kernel(ibb);
	wm_sampler = gen4_create_sampler(ibb,
					 SAMPLER_FILTER_NEAREST,
					 SAMPLER_EXTEND_NONE);
	wm = gen4_create_wm_state(ibb, wm_kernel, wm_sampler);

	cc_vp = gen4_create_cc_viewport(ibb);
	cc = gen4_create_cc_state(ibb, cc_vp);

	intel_bb_ptr_set(ibb, 0);

	gen4_emit_invariant(ibb);
	gen4_emit_state_base_address(ibb);
	gen4_emit_sip(ibb);
	gen4_emit_null_depth_buffer(ibb);

	gen4_emit_drawing_rectangle(ibb, dst);
	gen4_emit_binding_table(ibb, wm_table);
	gen4_emit_vertex_elements(ibb);
	gen4_emit_pipelined_pointers(ibb, vs, sf, wm, cc);
	gen4_emit_urb(ibb);

	gen4_emit_vertex_buffer(ibb);
	offset = gen4_emit_primitive(ibb);

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
