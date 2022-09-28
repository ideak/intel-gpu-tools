/*
 * Copyright © 2020 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include "i915_drm.h"
#include "igt_collection.h"
#include "i915_drm_local.h"

#ifndef INTEL_MEMORY_REGION_H
#define INTEL_MEMORY_REGION_H

#define I915_SYSTEM_MEMORY I915_MEMORY_CLASS_SYSTEM
#define I915_DEVICE_MEMORY I915_MEMORY_CLASS_DEVICE
#define I915_STOLEN_SYSTEM_MEMORY I915_MEMORY_CLASS_STOLEN_SYSTEM
#define I915_STOLEN_DEVICE_MEMORY I915_MEMORY_CLASS_STOLEN_DEVICE

#define INTEL_MEMORY_REGION_ID(type, instance) ((type) << 16u | (instance))
#define MEMORY_TYPE_FROM_REGION(r) ((r) >> 16u)
#define MEMORY_INSTANCE_FROM_REGION(r) ((r) & 0xffff)

#define IS_MEMORY_REGION_TYPE(region, type) \
	(MEMORY_TYPE_FROM_REGION(region) == type)

#define IS_DEVICE_MEMORY_REGION(region) \
	IS_MEMORY_REGION_TYPE(region, I915_MEMORY_CLASS_DEVICE)
#define IS_SYSTEM_MEMORY_REGION(region) \
	IS_MEMORY_REGION_TYPE(region, I915_MEMORY_CLASS_SYSTEM)

#define IS_STOLEN_MEMORY_REGION(region) \
	(IS_MEMORY_REGION_TYPE(region, I915_MEMORY_CLASS_STOLEN_SYSTEM) || \
	 IS_MEMORY_REGION_TYPE(region, I915_MEMORY_CLASS_STOLEN_DEVICE))

#define REGION_SMEM    INTEL_MEMORY_REGION_ID(I915_MEMORY_CLASS_SYSTEM, 0)
#define REGION_LMEM(n) INTEL_MEMORY_REGION_ID(I915_MEMORY_CLASS_DEVICE, (n))
#define REGION_STLN_SMEM(n) INTEL_MEMORY_REGION_ID(I915_MEMORY_CLASS_STOLEN_SYSTEM, (n))
#define REGION_STLN_LMEM(n) INTEL_MEMORY_REGION_ID(I915_MEMORY_CLASS_STOLEN_DEVICE, (n))

bool gem_has_query_support(int fd);

const char *get_memory_region_name(uint32_t region);

struct drm_i915_query_memory_regions *gem_get_query_memory_regions(int fd);

unsigned int gem_get_lmem_region_count(int fd);

bool gem_has_lmem(int fd);

int __gem_create_in_memory_region_list(int fd, uint32_t *handle, uint64_t *size, uint32_t flags,
				       const struct drm_i915_gem_memory_class_instance *mem_regions,
				       int num_regions);

uint32_t gem_create_in_memory_region_list(int fd, uint64_t size, uint32_t flags,
					  const struct drm_i915_gem_memory_class_instance *mem_regions,
					  int num_regions);

/*
 * XXX: the whole converting to class_instance thing is meant as a temporary
 * stop gap which should keep everything working, such that we don't have to
 * rewrite the world in one go to fit the new uAPI.
 */
#define __gem_create_in_memory_regions(fd, handle, size, regions...) ({ \
	unsigned int arr__[] = { regions }; \
	struct drm_i915_gem_memory_class_instance arr_query__[ARRAY_SIZE(arr__)]; \
	for (int i__  = 0; i__ < ARRAY_SIZE(arr_query__); ++i__) { \
		arr_query__[i__].memory_class = MEMORY_TYPE_FROM_REGION(arr__[i__]);  \
		arr_query__[i__].memory_instance = MEMORY_INSTANCE_FROM_REGION(arr__[i__]);  \
	} \
	__gem_create_in_memory_region_list(fd, handle, size, 0, arr_query__, ARRAY_SIZE(arr_query__)); \
})
#define gem_create_in_memory_regions(fd, size, regions...) ({ \
	unsigned int arr__[] = { regions }; \
	struct drm_i915_gem_memory_class_instance arr_query__[ARRAY_SIZE(arr__)]; \
	for (int i__  = 0; i__ < ARRAY_SIZE(arr_query__); ++i__) { \
		arr_query__[i__].memory_class = MEMORY_TYPE_FROM_REGION(arr__[i__]);  \
		arr_query__[i__].memory_instance = MEMORY_INSTANCE_FROM_REGION(arr__[i__]);  \
	} \
	gem_create_in_memory_region_list(fd, size, 0, arr_query__, ARRAY_SIZE(arr_query__)); \
})

/*
 * Create an object that requires CPU access. This only becomes interesting on
 * platforms that have a small BAR for LMEM CPU access. Without this the object
 * might need to be migrated when CPU faulting the object, or if that is not
 * possible we hit SIGBUS. Most users should be fine with this. If enabled the
 * kernel will never allocate this object in the non-CPU visible portion of
 * LMEM.
 *
 * Underneath this just enables the I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS
 * flag, if we also have an LMEM placement. Also since the kernel requires SMEM
 * as a potential placement, we automatically attach that as a possible
 * placement, if not already provided. If this happens to be an SMEM-only
 * placement then we don't supply the flag, and instead just treat as normal
 * allocation.
 */

#define __gem_create_with_cpu_access_in_memory_regions(fd, handle, size, regions...) ({ \
	unsigned int arr__[] = { regions }; \
	struct drm_i915_gem_memory_class_instance arr_query__[ARRAY_SIZE(arr__) + 1]; \
	int i__, arr_query_size__ = ARRAY_SIZE(arr__); \
	uint32_t ext_flags__ = 0; \
	bool ext_found_smem__ = false; \
	for (i__  = 0; i__ < arr_query_size__; ++i__) { \
		arr_query__[i__].memory_class = MEMORY_TYPE_FROM_REGION(arr__[i__]);  \
		if (arr_query__[i__].memory_class == I915_MEMORY_CLASS_DEVICE) \
			ext_flags__ = I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS; \
		else \
			ext_found_smem__ = true; \
		arr_query__[i__].memory_instance = MEMORY_INSTANCE_FROM_REGION(arr__[i__]);  \
	} \
	if (ext_flags__ && !ext_found_smem__) { \
		arr_query__[i__].memory_class = I915_MEMORY_CLASS_SYSTEM; \
		arr_query__[i__].memory_instance = 0; \
		arr_query_size__++; \
	} \
	__gem_create_in_memory_region_list(fd, handle, size, ext_flags__, arr_query__, arr_query_size__); \
})
#define gem_create_with_cpu_access_in_memory_regions(fd, size, regions...) ({ \
	unsigned int arr__[] = { regions }; \
	struct drm_i915_gem_memory_class_instance arr_query__[ARRAY_SIZE(arr__) + 1]; \
	int i__, arr_query_size__ = ARRAY_SIZE(arr__); \
	uint32_t ext_flags__ = 0; \
	bool ext_found_smem__ = false; \
	for (i__  = 0; i__ < arr_query_size__; ++i__) { \
		arr_query__[i__].memory_class = MEMORY_TYPE_FROM_REGION(arr__[i__]);  \
		if (arr_query__[i__].memory_class == I915_MEMORY_CLASS_DEVICE) \
			ext_flags__ = I915_GEM_CREATE_EXT_FLAG_NEEDS_CPU_ACCESS; \
		else \
			ext_found_smem__ = true; \
		arr_query__[i__].memory_instance = MEMORY_INSTANCE_FROM_REGION(arr__[i__]);  \
	} \
	if (ext_flags__ && !ext_found_smem__) { \
		arr_query__[i__].memory_class = I915_MEMORY_CLASS_SYSTEM; \
		arr_query__[i__].memory_instance = 0; \
		arr_query_size__++; \
	} \
	gem_create_in_memory_region_list(fd, size, ext_flags__, arr_query__, arr_query_size__); \
})

struct igt_collection *
__get_memory_region_set(struct drm_i915_query_memory_regions *regions,
			uint32_t *mem_regions_type,
			int num_regions);

/*
 * Helper macro to create igt_collection which contains all memory regions
 * which matches mem_region_types array. Useful to filter out stolen memory
 * from accessible memory regions.
 */
#define get_memory_region_set(regions, mem_region_types...) ({ \
	unsigned int arr__[] = { mem_region_types }; \
	__get_memory_region_set(regions, arr__, ARRAY_SIZE(arr__)); \
})

struct gem_memory_region {
	struct gem_memory_region *next;
	char *name;

	struct drm_i915_gem_memory_class_instance ci;
	uint64_t size;
	uint64_t cpu_size;
};

struct igt_collection *
get_dma_buf_mmap_supported_set(int i915, struct igt_collection *set);

char *memregion_dynamic_subtest_name(struct igt_collection *set);

void intel_dump_gpu_meminfo(const struct drm_i915_query_memory_regions *info);

uint32_t gpu_meminfo_region_count(const struct drm_i915_query_memory_regions *info,
				  uint16_t region_class);
uint64_t gpu_meminfo_region_total_size(const struct drm_i915_query_memory_regions *info,
				       uint16_t region_class);
uint64_t gpu_meminfo_region_total_available(const struct drm_i915_query_memory_regions *info,
					    uint16_t region_type);

uint64_t gpu_meminfo_region_size(const struct drm_i915_query_memory_regions *info,
				 uint16_t memory_class,
				 uint16_t memory_instance);
uint64_t gpu_meminfo_region_available(const struct drm_i915_query_memory_regions *info,
				      uint16_t memory_class,
				      uint16_t memory_instance);

uint64_t gem_detect_min_start_offset_for_region(int i915, uint32_t region);
uint64_t gem_detect_safe_start_offset(int i915);
uint64_t gem_detect_min_alignment_for_regions(int i915, uint32_t region1, uint32_t region2);
uint64_t gem_detect_safe_alignment(int i915);

struct gem_memory_region *__gem_get_memory_regions(int i915);
struct gem_memory_region *
__gem_next_memory_region(struct gem_memory_region *r);

#define for_each_memory_region(r__, fd__) for (struct gem_memory_region *r__ = __gem_get_memory_regions(fd__); r__; r__ = __gem_next_memory_region(r__))

#endif /* INTEL_MEMORY_REGION_H */
