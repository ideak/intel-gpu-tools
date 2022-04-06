/*
 * Copyright 2019 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef IGT_AMD_H
#define IGT_AMD_H

#include <stdint.h>
#include "igt.h"
#include "igt_fb.h"
#include "igt_psr.h"

/* Read & Write DSC parameters */
#define DEBUGFS_DSC_CLOCK_EN "dsc_clock_en"
#define DEBUGFS_DSC_SLICE_WIDTH "dsc_slice_width"
#define DEBUGFS_DSC_SLICE_HEIGHT "dsc_slice_height"
#define DEBUGFS_DSC_BITS_PER_PIXEL "dsc_bits_per_pixel"
/* Read only DSC parameters */
#define DEBUGFS_DSC_PIC_WIDTH "dsc_pic_width"
#define DEBUGFS_DSC_PIC_HEIGHT "dsc_pic_height"
#define DEBUGFS_DSC_CHUNK_SIZE "dsc_chunk_size"
#define DEBUGFS_DSC_SLICE_BPG "dsc_slice_bpg"
#define DEBUGFS_DSC_FEC_SUPPORT "dp_dsc_fec_support"

#define DEBUGFS_DP_LINK_SETTINGS "link_settings"
#define DEBUGFS_HPD_TRIGGER "trigger_hotplug"

/* eDP related */
#define DEBUGFS_EDP_ILR_SETTING "ilr_setting"
#define MAX_SUPPORTED_ILR 8
#define DEBUGFS_EDP_PSR_CAP	"psr_capability"
#define DEBUGFS_EDP_PSR_STATE	"psr_state"

/* amdgpu DM interface entries */
#define DEBUGFS_DM_VISUAL_CONFIRM "amdgpu_dm_visual_confirm"

enum amd_dsc_clock_force {
	DSC_AUTOMATIC = 0,
	DSC_FORCE_ON,
	DSC_FORCE_OFF,
};

enum dc_lane_count {
	LANE_COUNT_UNKNOWN = 0,
	LANE_COUNT_ONE = 1,
	LANE_COUNT_TWO = 2,
	LANE_COUNT_FOUR = 4,
	LANE_COUNT_EIGHT = 8,
	LANE_COUNT_DP_MAX = LANE_COUNT_FOUR
};

/* This is actually a reference clock (27MHz) multiplier
 * 162MBps bandwidth for 1.62GHz like rate,
 * 270MBps for 2.70GHz,
 * 324MBps for 3.24Ghz,
 * 540MBps for 5.40GHz
 * 810MBps for 8.10GHz
 */
enum dc_link_rate {
	LINK_RATE_UNKNOWN = 0,
	LINK_RATE_LOW = 0x06,		// Rate_1 (RBR)	- 1.62 Gbps/Lane
	LINK_RATE_RATE_2 = 0x08,	// Rate_2		- 2.16 Gbps/Lane
	LINK_RATE_RATE_3 = 0x09,	// Rate_3		- 2.43 Gbps/Lane
	LINK_RATE_HIGH = 0x0A,		// Rate_4 (HBR)	- 2.70 Gbps/Lane
	LINK_RATE_RBR2 = 0x0C,		// Rate_5 (RBR2)- 3.24 Gbps/Lane
	LINK_RATE_RATE_6 = 0x10,	// Rate_6		- 4.32 Gbps/Lane
	LINK_RATE_HIGH2 = 0x14,		// Rate_7 (HBR2)- 5.40 Gbps/Lane
	LINK_RATE_HIGH3 = 0x1E		// Rate_8 (HBR3)- 8.10 Gbps/Lane
};

enum dc_link_training_type {
	LINK_TRAINING_DEFAULT = 0,
	LINK_TRAINING_SLOW = 0,
	LINK_TRAINING_FAST,
	LINK_TRAINING_NO_PATTERN
};

/*
 * enumeration of PSR STATE below should be aligned to the upstreamed
 * amdgpu kernel driver 'enum dc_psr_state' in dc_type.h
 */
enum amdgpu_psr_state {
	PSR_STATE0 = 0x0,
	PSR_STATE1,
	PSR_STATE1a,
	PSR_STATE2,
	PSR_STATE2a,
	PSR_STATE2b,
	PSR_STATE3,
	PSR_STATE3Init,
	PSR_STATE4,
	PSR_STATE4a,
	PSR_STATE4b,
	PSR_STATE4c,
	PSR_STATE4d,
	PSR_STATE5,
	PSR_STATE5a,
	PSR_STATE5b,
	PSR_STATE5c,
	PSR_STATE_INVALID = 0xFF
};

/*
 * enumeration of amdgpu DC visual confirm debug option
 * aligned to the upstreamed amdgpu kernel driver 'enum visual_confirm' in dc.h
 */
enum amdgpu_debug_visual_confirm {
	VISUAL_CONFIRM_DISABLE	= 0,
	VISUAL_CONFIRM_SURFACE	= 1,
	VISUAL_CONFIRM_HDR	= 2,
	VISUAL_CONFIRM_MPCTREE	= 4,
	VISUAL_CONFIRM_PSR	= 5,
	VISUAL_CONFIRM_SWIZZLE	= 9
};

uint32_t igt_amd_create_bo(int fd, uint64_t size);
void *igt_amd_mmap_bo(int fd, uint32_t handle, uint64_t size, int prot);
unsigned int igt_amd_compute_offset(unsigned int* swizzle_pattern,
				       unsigned int x, unsigned int y);
unsigned int igt_amd_fb_get_blk_size_table_idx(unsigned int bpp);
void igt_amd_fb_calculate_tile_dimension(unsigned int bpp,
				       unsigned int *width, unsigned int *height);
uint32_t igt_amd_fb_tiled_offset(unsigned int bpp, unsigned int x_input,
				       unsigned int y_input, unsigned int width_input);
void igt_amd_fb_to_tiled(struct igt_fb *dst, void *dst_buf, struct igt_fb *src,
				       void *src_buf, unsigned int plane);
void igt_amd_fb_convert_plane_to_tiled(struct igt_fb *dst, void *dst_buf,
				       struct igt_fb *src, void *src_buf);
bool igt_amd_is_tiled(uint64_t modifier);

/* IGT DSC helper functions */
bool is_dp_dsc_supported(int drm_fd, char *connector_name);
bool is_dp_fec_supported(int drm_fd, char *connector_name);
void igt_amd_require_dsc(igt_display_t *display, int drm_fd);
int igt_amd_read_dsc_clock_status(int drm_fd, char *connector_name);
void igt_amd_write_dsc_clock_en(int drm_fd, char *connector_name, int dsc_force);
void igt_amd_write_dsc_param_slice_height(int drm_fd, char *connector_name, int slice_height);
int igt_amd_read_dsc_param_slice_height(int drm_fd, char *connector_name);
void igt_amd_write_dsc_param_slice_width(int drm_fd, char *connector_name, int slice_width);
int igt_amd_read_dsc_param_slice_width(int drm_fd, char *connector_name);
void igt_amd_write_dsc_param_bpp(int drm_fd, char *connector_name, int bpp);
int igt_amd_read_dsc_param_bpp(int drm_fd, char *connector_name);
int igt_amd_read_dsc_param_pic_width(int drm_fd, char *connector_name);
int igt_amd_read_dsc_param_pic_height(int drm_fd, char *connector_name);
int igt_amd_read_dsc_param_chunk_size(int drm_fd, char *connector_name);
int igt_amd_read_dsc_param_slice_bpg(int drm_fd, char *connector_name);

/* IGT HPD helper functions */
void igt_amd_require_hpd(igt_display_t *display, int drm_fd);
int igt_amd_trigger_hotplug(int drm_fd, char *connector_name);

/* IGT link helper functions */
void igt_amd_read_link_settings(
	int drm_fd, char *connector_name, int *lane_count, int *link_rate, int *link_spread);
void igt_amd_write_link_settings(
	int drm_fd, char *connector_name, enum dc_lane_count lane_count,
	enum dc_link_rate link_rate, enum dc_link_training_type training_type);
bool igt_amd_output_has_link_settings(int drm_fd, char *connector_name);

/* eDP debugfs helpers */
void igt_amd_read_ilr_setting(
	int drm_fd, char *connector_name, int *supported_ilr);
void igt_amd_write_ilr_setting(
	int drm_fd, char *connector_name, enum dc_lane_count lane_count,
	uint8_t link_rate_set);
bool igt_amd_output_has_ilr_setting(int drm_fd, char *connector_name);
bool igt_amd_output_has_psr_cap(int drm_fd, char *connector_name);
bool igt_amd_psr_support_sink(int drm_fd, char *connector_name, enum psr_mode mode);
bool igt_amd_psr_support_drv(int drm_fd, char *connector_name, enum psr_mode mode);
bool igt_amd_output_has_psr_state(int drm_fd, char *connector_name);
int  igt_amd_read_psr_state(int drm_fd, char *connector_name);

/* DM interface helpers */
bool igt_amd_has_visual_confirm(int drm_fd);
int  igt_amd_get_visual_confirm(int drm_fd);
bool igt_amd_set_visual_confirm(int drm_fd, enum amdgpu_debug_visual_confirm option);
#endif /* IGT_AMD_H */
