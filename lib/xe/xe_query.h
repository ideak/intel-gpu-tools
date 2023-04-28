/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 *
 * Authors:
 *    Matthew Brost <matthew.brost@intel.com>
 */

#ifndef XE_QUERY_H
#define XE_QUERY_H

#include <stdint.h>
#include <xe_drm.h>
#include "igt_list.h"

#define SZ_4K	0x1000
#define SZ_64K	0x10000

#define XE_DEFAULT_ALIGNMENT           SZ_4K
#define XE_DEFAULT_ALIGNMENT_64K       SZ_64K

struct xe_device {
	/** @fd: xe fd */
	int fd;

	/** @config: xe configuration */
	struct drm_xe_query_config *config;

	/** @gts: gt info */
	struct drm_xe_query_gts *gts;

	/** @number_gt: number of gt */
	unsigned int number_gt;

	/** @gts: bitmask of all memory regions */
	uint64_t memory_regions;

	/** @hw_engines: array of hardware engines */
	struct drm_xe_engine_class_instance *hw_engines;

	/** @number_hw_engines: length of hardware engines array */
	unsigned int number_hw_engines;

	/** @mem_usage: regions memory information and usage */
	struct drm_xe_query_mem_usage *mem_usage;

	/** @vram_size: array of vram sizes for all gts */
	uint64_t *vram_size;

	/** @default_alignment: safe alignment regardless region location */
	uint32_t default_alignment;

	/** @has_vram: true if gpu has vram, false if system memory only */
	bool has_vram;

	/** @va_bits: va length in bits */
	uint32_t va_bits;

	/** @dev_id: Device id of xe device */
	uint16_t dev_id;
};

#define xe_for_each_hw_engine(__fd, __hwe) \
	for (int __i = 0; __i < xe_number_hw_engines(__fd) && \
	     (__hwe = xe_hw_engine(__fd, __i)); ++__i)
#define xe_for_each_hw_engine_class(__class) \
	for (__class = 0; __class < DRM_XE_ENGINE_CLASS_COMPUTE + 1; \
	     ++__class)
#define xe_for_each_gt(__fd, __gt) \
	for (__gt = 0; __gt < xe_number_gt(__fd); ++__gt)

#define xe_for_each_mem_region(__fd, __memreg, __r) \
	for (uint64_t __i = 0; __i < igt_fls(__memreg); __i++) \
		for_if(__r = (__memreg & (1ull << __i)))

#define XE_IS_CLASS_SYSMEM(__region) ((__region)->mem_class == XE_MEM_REGION_CLASS_SYSMEM)
#define XE_IS_CLASS_VRAM(__region) ((__region)->mem_class == XE_MEM_REGION_CLASS_VRAM)

unsigned int xe_number_gt(int fd);
uint64_t all_memory_regions(int fd);
uint64_t system_memory(int fd);
uint64_t vram_memory(int fd, int gt);
uint64_t vram_if_possible(int fd, int gt);
struct drm_xe_engine_class_instance *xe_hw_engines(int fd);
struct drm_xe_engine_class_instance *xe_hw_engine(int fd, int idx);
struct drm_xe_query_mem_region *xe_mem_region(int fd, uint64_t region);
const char *xe_region_name(uint64_t region);
uint32_t xe_min_page_size(int fd, uint64_t region);
unsigned int xe_number_hw_engines(int fd);
bool xe_has_vram(int fd);
//uint64_t xe_vram_size(int fd);
uint64_t xe_vram_size(int fd, int gt);
uint32_t xe_get_default_alignment(int fd);
uint32_t xe_va_bits(int fd);
uint16_t xe_dev_id(int fd);
bool xe_supports_faults(int fd);
const char *xe_engine_class_string(uint32_t engine_class);

struct xe_device *xe_device_get(int fd);
void xe_device_put(int fd);

#endif	/* XE_QUERY_H */
