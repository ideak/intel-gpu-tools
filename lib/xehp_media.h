/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef XEHP_MEDIA_H
#define XEHP_MEDIA_H

#include <stdint.h>
#include "surfaceformat.h"
#include "gen7_media.h"

#define GFXPIPE_XEHP(Pipeline, Opcode, Subopcode) ((3 << 29) |		\
						  ((Pipeline) << 27) |	\
						  ((Opcode) << 24) |	\
						  ((Subopcode) << 18))

#define XEHP_STATE_COMPUTE_MODE		GFXPIPE(0, 1, 5)
#define XEHP_CFE_STATE			GFXPIPE_XEHP(2, 2, 0)
#define XEHP_COMPUTE_WALKER		GFXPIPE_XEHP(2, 2, 2)

#define BITRANGE(start, end) (end - start + 1)

struct xehp_interface_descriptor_data {
	struct {
		uint32_t pad0: BITRANGE(0, 5);
		uint32_t kernel_start_pointer: BITRANGE(6, 31);
	} desc0;

	struct {
		uint32_t kernel_start_pointer_high: BITRANGE(0, 15);
		uint32_t pad0: BITRANGE(16, 31);
	} desc1;

	struct {
		uint32_t pad0: BITRANGE(0, 6);
		uint32_t software_exception_enable: BITRANGE(7, 7);
		uint32_t pad1: BITRANGE(8, 10);
		uint32_t maskstack_exception_enable: BITRANGE(11, 11);
		uint32_t pad2: BITRANGE(12, 12);
		uint32_t illegal_opcode_exception_enable: BITRANGE(13, 13);
		uint32_t pad3: BITRANGE(14, 15);
		uint32_t floating_point_mode: BITRANGE(16, 16);
		uint32_t pad4: BITRANGE(17, 17);
		uint32_t single_program_flow: BITRANGE(18, 18);
		uint32_t denorm_mode: BITRANGE(19, 19);
		uint32_t thread_preemption_disable: BITRANGE(20, 20);
		uint32_t pad5: BITRANGE(21, 31);
	} desc2;

	struct {
		uint32_t pad0: BITRANGE(0, 1);
		uint32_t sampler_count: BITRANGE(2, 4);
		uint32_t sampler_state_pointer: BITRANGE(5, 31);
	} desc3;

	struct {
		uint32_t binding_table_entry_count: BITRANGE(0, 4);
		uint32_t binding_table_pointer: BITRANGE(5, 20);
		uint32_t pad0: BITRANGE(21, 31);
	} desc4;

	struct {
		uint32_t num_threads_in_tg: BITRANGE(0, 9);
		uint32_t pad0: BITRANGE(10, 15);
		uint32_t shared_local_memory_size: BITRANGE(16, 20);
		uint32_t barrier_enable: BITRANGE(21, 21);
		uint32_t rounding_mode: BITRANGE(22, 23);
		uint32_t pad1: BITRANGE(24, 26);
		uint32_t thread_group_dispatch_size: BITRANGE(27, 27);
		uint32_t pad2: BITRANGE(28, 31);
	} desc5;

	struct {
		uint32_t pad0;
	} desc6;

	struct {
		uint32_t pad0;
	} desc7;
};

struct xehp_surface_state {
	struct {
		uint32_t cube_pos_z: BITRANGE(0, 0);
		uint32_t cube_neg_z: BITRANGE(1, 1);
		uint32_t cube_pos_y: BITRANGE(2, 2);
		uint32_t cube_neg_y: BITRANGE(3, 3);
		uint32_t cube_pos_x: BITRANGE(4, 4);
		uint32_t cube_neg_x: BITRANGE(5, 5);
		uint32_t media_boundary_pixel_mode: BITRANGE(6, 7);
		uint32_t render_cache_read_write: BITRANGE(8, 8);
		uint32_t sampler_l2_bypass_disable: BITRANGE(9, 9);
		uint32_t vert_line_stride_ofs: BITRANGE(10, 10);
		uint32_t vert_line_stride: BITRANGE(11, 11);
		uint32_t tiled_mode: BITRANGE(12, 13);
		uint32_t horizontal_alignment: BITRANGE(14, 15);
		uint32_t vertical_alignment: BITRANGE(16, 17);
		uint32_t surface_format: BITRANGE(18, 26);     /**< BRW_SURFACEFORMAT_x */
		uint32_t astc_enable: BITRANGE(27, 27);
		uint32_t is_array: BITRANGE(28, 28);
		uint32_t surface_type: BITRANGE(29, 31);       /**< BRW_SURFACE_1D/2D/3D/CUBE */
	} ss0;

	struct {
		uint32_t qpitch: BITRANGE(0, 14);
		uint32_t sample_tap_discard_disable: BITRANGE(15, 15);
		uint32_t pad0: BITRANGE(16, 16);
		uint32_t double_fetch_disable: BITRANGE(17, 17);
		uint32_t corner_texel_mode: BITRANGE(18, 18);
		uint32_t base_mip_level: BITRANGE(19, 23);
		uint32_t memory_object_control: BITRANGE(24, 30);
		uint32_t unorm_path_in_color_pipe: BITRANGE(31, 31);
	} ss1;

	struct {
		uint32_t width: BITRANGE(0, 13);
		uint32_t pad0: BITRANGE(14, 15);
		uint32_t height: BITRANGE(16, 29);
		uint32_t pad1: BITRANGE(30, 30);
		uint32_t depth_stencil_resource: BITRANGE(31, 31);
	} ss2;

	struct {
		uint32_t pitch: BITRANGE(0, 17);
		uint32_t null_probing_enable: BITRANGE(18, 18);
		uint32_t standard_tiling_mode_ext: BITRANGE(19, 19);
		uint32_t pad0: BITRANGE(20, 20);
		uint32_t depth: BITRANGE(21, 31);
	} ss3;

	struct {
		uint32_t multisample_position_palette_index: BITRANGE(0, 2);
		uint32_t num_multisamples: BITRANGE(3, 5);
		uint32_t multisampled_surface_storage_format: BITRANGE(6, 6);
		uint32_t render_target_view_extent: BITRANGE(7, 17);
		uint32_t min_array_element: BITRANGE(18, 28);
		uint32_t rotation: BITRANGE(29, 30);
		uint32_t decompress_in_l3: BITRANGE(31, 31);
	} ss4;

	struct {
		uint32_t mip_count: BITRANGE(0, 3);
		uint32_t surface_min_lod: BITRANGE(4, 7);
		uint32_t mip_tail_start_lod: BITRANGE(8, 11);
		uint32_t yuv_bpt: BITRANGE(12, 13);
		uint32_t coherency_type: BITRANGE(14, 15);
		uint32_t pad0: BITRANGE(16, 17);
		uint32_t tiled_resource_mode: BITRANGE(18, 19);
		uint32_t ewa_disable_for_cube: BITRANGE(20, 20);
		uint32_t y_offset: BITRANGE(21, 23);
		uint32_t pad1: BITRANGE(24, 24);
		uint32_t x_offset: BITRANGE(25, 31);
	} ss5;

	struct {
		uint32_t pad; /* Multisample Control Surface stuff */
	} ss6;

	struct {
		uint32_t resource_min_lod: BITRANGE(0, 11);
		uint32_t pad0: BITRANGE(12, 13);
		uint32_t disable_support_for_multigpu_atomics: BITRANGE(14, 14);
		uint32_t disable_support_for_multigpu_partwrite: BITRANGE(15, 15);
		uint32_t shader_channel_select_a: BITRANGE(16, 18);
		uint32_t shader_channel_select_b: BITRANGE(19, 21);
		uint32_t shader_channel_select_g: BITRANGE(22, 24);
		uint32_t shader_channel_select_r: BITRANGE(25, 27);
		uint32_t pad1: BITRANGE(28, 29);
		uint32_t memory_compression_enable: BITRANGE(30, 30);
		uint32_t memory_compression_mode: BITRANGE(31, 31);
	} ss7;

	struct {
		uint32_t base_addr_lo;
	} ss8;

	struct {
		uint32_t base_addr_hi;
	} ss9;

	struct {
		uint32_t pad0: BITRANGE(0, 11);
		uint32_t aux_base_addr_lo: BITRANGE(12, 31);
	} ss10;

	struct {
		uint32_t aux_base_addr_hi;
	} ss11;

	struct {
		uint32_t compression_format: BITRANGE(0, 4);
		uint32_t clear_address_lo: BITRANGE(5, 31);
	} ss12;

	struct {
		uint32_t clear_address_hi: BITRANGE(0, 15);
		uint32_t pad0: BITRANGE(16, 31);
	} ss13;

	struct {
		uint32_t reserved;
	} ss14;

	struct {
		uint32_t reserved;
	} ss15;
};

#endif /* XEHP_MEDIA_H */
