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

#include <linux/limits.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "i915/gem_create.h"
#include "intel_reg.h"
#include "drmtest.h"
#include "ioctl_wrappers.h"
#include "igt_dummyload.h"
#include "igt_gt.h"
#include "igt_params.h"
#include "igt_sysfs.h"
#include "intel_chipset.h"
#include "igt_collection.h"
#include "igt_device.h"

#include "i915/intel_memory_region.h"

#define i915_query_items(fd, items, n_items) do { \
		igt_assert_eq(__i915_query_items(fd, items, n_items), 0); \
		errno = 0; \
	} while (0)
#define i915_query_items_err(fd, items, n_items, err) do { \
		igt_assert_eq(__i915_query_items(fd, items, n_items), -err); \
	} while (0)

static int
__i915_query(int fd, struct drm_i915_query *q)
{
	if (igt_ioctl(fd, DRM_IOCTL_I915_QUERY, q))
		return -errno;
	return 0;
}

static int
__i915_query_items(int fd, struct drm_i915_query_item *items, uint32_t n_items)
{
	struct drm_i915_query q = {
		.num_items = n_items,
		.items_ptr = to_user_pointer(items),
	};
	return __i915_query(fd, &q);
}

bool gem_has_query_support(int fd)
{
	struct drm_i915_query query = {};

	return __i915_query(fd, &query) == 0;
}

const char *get_memory_region_name(uint32_t region)
{
	uint16_t class = MEMORY_TYPE_FROM_REGION(region);

	switch (class) {
	case I915_MEMORY_CLASS_SYSTEM:
		return "smem";
	case I915_MEMORY_CLASS_DEVICE:
		return "lmem";
	}
	igt_assert_f(false, "Unknown memory region");
}

/**
 *  gem_get_batch_size:
 *  @fd: open i915 drm file descriptor
 *  @mem_region_type: used memory_region type
 *
 *  With introduction of LMEM we observe different page sizes for those two
 *  memory regions. Without this helper funtion we may be prone to forget
 *  about setting proper page size.
 */
uint32_t gem_get_batch_size(int fd, uint8_t mem_region_type)
{
	return (mem_region_type == I915_MEMORY_CLASS_DEVICE) ? 65536 : 4096;
}

/**
 * gem_get_query_memory_regions:
 * @fd: open i915 drm file descriptor
 *
 * This function wraps query mechanism for memory regions.
 *
 * Returns: Filled struct with available memory regions.
 */
struct drm_i915_query_memory_regions *gem_get_query_memory_regions(int fd)
{
	struct drm_i915_query_item item;
	struct drm_i915_query_memory_regions *query_info;

	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_MEMORY_REGIONS;
	i915_query_items(fd, &item, 1);

	query_info = calloc(1, item.length);

	item.data_ptr = to_user_pointer(query_info);
	i915_query_items(fd, &item, 1);

	return query_info;
}

/**
 * gem_get_lmem_region_count:
 * @fd: open i915 drm file descriptor
 *
 * Helper function to check how many lmem regions are available on device.
 *
 * Returns: Number of found lmem regions.
 */
uint8_t gem_get_lmem_region_count(int fd)
{
	struct drm_i915_query_memory_regions *query_info;
	uint8_t num_regions;
	uint8_t lmem_regions = 0;

	query_info = gem_get_query_memory_regions(fd);
	num_regions = query_info->num_regions;

	for (int i = 0; i < num_regions; i++) {
		if (query_info->regions[i].region.memory_class == I915_MEMORY_CLASS_DEVICE)
			lmem_regions += 1;
	}
	free(query_info);

	return lmem_regions;
}

/**
 * gem_has_lmem:
 * @fd: open i915 drm file descriptor
 *
 * Helper function to check if lmem is available on device.
 *
 * Returns: True if at least one lmem region was found.
 */
bool gem_has_lmem(int fd)
{
	return gem_get_lmem_region_count(fd) > 0;
}

/* A version of gem_create_in_memory_region_list which can be allowed to
   fail so that the object creation can be retried */
int __gem_create_in_memory_region_list(int fd, uint32_t *handle, uint64_t size,
				       struct drm_i915_gem_memory_class_instance *mem_regions,
				       int num_regions)
{
	struct drm_i915_gem_create_ext_memory_regions ext_regions = {
		.base = { .name = I915_GEM_CREATE_EXT_MEMORY_REGIONS },
		.num_regions = num_regions,
		.regions = to_user_pointer(mem_regions),
	};

	return __gem_create_ext(fd, &size, handle, &ext_regions.base);
}

/* gem_create_in_memory_region_list:
 * @fd: opened i915 drm file descriptor
 * @size: requested size of the buffer
 * @mem_regions: memory regions array (priority list)
 * @num_regions: @mem_regions length
 */
uint32_t gem_create_in_memory_region_list(int fd, uint64_t size,
					  struct drm_i915_gem_memory_class_instance *mem_regions,
					  int num_regions)
{
	uint32_t handle;
	int ret = __gem_create_in_memory_region_list(fd, &handle, size,
						     mem_regions, num_regions);
	igt_assert_eq(ret, 0);
	return handle;
}

static bool __region_belongs_to_regions_type(struct drm_i915_gem_memory_class_instance region,
					     uint32_t *mem_regions_type,
					     int num_regions)
{
	for (int i = 0; i < num_regions; i++)
		if (mem_regions_type[i] == region.memory_class)
			return true;
	return false;
}

struct igt_collection *
__get_memory_region_set(struct drm_i915_query_memory_regions *regions,
			uint32_t *mem_regions_type,
			int num_regions)
{
	struct drm_i915_gem_memory_class_instance region;
	struct igt_collection *set;
	int count = 0, pos = 0;

	for (int i = 0; i < regions->num_regions; i++) {
		region = regions->regions[i].region;
		if (__region_belongs_to_regions_type(region,
						     mem_regions_type,
						     num_regions))
			count++;
	}

	set = igt_collection_create(count);

	for (int i = 0; i < regions->num_regions; i++) {
		region = regions->regions[i].region;
		if (__region_belongs_to_regions_type(region,
						     mem_regions_type,
						     num_regions))
			igt_collection_set_value(set, pos++,
						 INTEL_MEMORY_REGION_ID(region.memory_class,
									region.memory_instance));
	}

	igt_assert(count == pos);

	return set;
}

/**
  * memregion_dynamic_subtest_name:
  * @igt_collection: memory region collection
  *
  * Function iterates over all memory regions inside the collection (keeped
  * in the value field) and generates the name which can be used during dynamic
  * subtest creation.
  *
  * Returns: newly allocated string, has to be freed by caller. Asserts if
  * caller tries to create a name using empty collection.
  */
char *memregion_dynamic_subtest_name(struct igt_collection *set)
{
	struct igt_collection_data *data;
	char *name, *p;
	uint32_t region, len;

	igt_assert(set && set->size);
	/* enough for "name%d-" * n */
	len = set->size * 8;
	p = name = malloc(len);
	igt_assert(name);

	for_each_collection_data(data, set) {
		int r;

		region = data->value;
		if (IS_DEVICE_MEMORY_REGION(region))
			r = snprintf(p, len, "%s%d-",
				     get_memory_region_name(region),
				     MEMORY_INSTANCE_FROM_REGION(region));
		else
			r = snprintf(p, len, "%s-",
				     get_memory_region_name(region));

		igt_assert(r > 0);
		p += r;
		len -= r;
	}

	/* remove last '-' */
	*(p - 1) = 0;

	return name;
}

/**
 * intel_dump_gpu_meminfo:
 * @info: pointer to drm_i915_query_memory_regions structure
 *
 * Outputs memory regions and their sizes.
 */
void intel_dump_gpu_meminfo(struct drm_i915_query_memory_regions *info)
{
	int i;

	igt_assert(info);

	igt_info("GPU meminfo:\n");

	for (i = 0; i < info->num_regions; i++) {
		uint32_t region = INTEL_MEMORY_REGION_ID(info->regions[i].region.memory_class,
							 info->regions[i].region.memory_instance);
		const char *name = get_memory_region_name(region);

		igt_info("- %s [%d] memory [size: 0x%llx, available: 0x%llx]\n",
			 name, info->regions[i].region.memory_instance,
			 info->regions[i].probed_size,
			 info->regions[i].unallocated_size);
	}
}

/**
 * gpu_meminfo_region_count:
 * @info: pointer to drm_i915_query_memory_regions structure
 * @memory_class: memory region class
 *
 * Returns: number of regions for type @memory_class
 */
uint32_t gpu_meminfo_region_count(struct drm_i915_query_memory_regions *info,
				  uint16_t memory_class)
{
	uint32_t num = 0;
	int i;

	igt_assert(info);

	for (i = 0; i < info->num_regions; i++)
		if (info->regions[i].region.memory_class == memory_class)
			num++;

	return num;
}

/**
 * gpu_meminfo_region_total_size:
 * @info: pointer to drm_i915_query_memory_regions structure
 * @memory_class: memory region class
 *
 * Returns: total size of all regions which are type @memory_class, -1 when the
 * size of at least one region is unknown
 */
uint64_t gpu_meminfo_region_total_size(struct drm_i915_query_memory_regions *info,
				       uint16_t memory_class)
{
	uint64_t total = 0;
	int i;

	igt_assert(info);

	for (i = 0; i < info->num_regions; i++)
		if (info->regions[i].region.memory_class == memory_class) {
			if (info->regions[i].probed_size == -1)
				return -1;

			total += info->regions[i].probed_size;
		}

	return total;
}

/**
 * gpu_meminfo_region_total_available:
 * @info: pointer to drm_i915_query_memory_regions structure
 * @memory_class: memory region class
 *
 * Returns: available size of all regions which are type @memory_class, -1 when
 * the size of at least one region cannot be estimated
 */
uint64_t gpu_meminfo_region_total_available(struct drm_i915_query_memory_regions *info,
					    uint16_t memory_class)
{
	uint64_t avail = 0;
	int i;

	igt_assert(info);

	for (i = 0; i < info->num_regions; i++)
		if (info->regions[i].region.memory_class == memory_class) {
			if (info->regions[i].unallocated_size == -1)
				return -1;

			avail += info->regions[i].unallocated_size;
		}

	return avail;
}

/**
 * gpu_meminfo_region_size:
 * @info: pointer to drm_i915_query_memory_regions structure
 * @memory_class: memory region class
 * @memory_instance: memory region instance
 *
 * Returns: available size of @memory_instance which type is @memory_class, -1
 * when the size is unknown
 */
uint64_t gpu_meminfo_region_size(struct drm_i915_query_memory_regions *info,
				 uint16_t memory_class,
				 uint16_t memory_instance)
{
	int i;

	igt_assert(info);

	for (i = 0; i < info->num_regions; i++)
		if (info->regions[i].region.memory_class == memory_class &&
		     info->regions[i].region.memory_instance == memory_instance)
			return info->regions[i].probed_size;

	return 0;
}

/**
 * gpu_meminfo_region_available:
 * @info: pointer to drm_i915_query_memory_regions structure
 * @memory_class: memory region class
 * @memory_instance: memory region instance
 *
 * Returns: available size of @memory_instance region which type is
 * @memory_class, -1 when the size cannot be estimated
 */
uint64_t gpu_meminfo_region_available(struct drm_i915_query_memory_regions *info,
				      uint16_t memory_class,
				      uint16_t memory_instance)
{
	int i;

	igt_assert(info);

	for (i = 0; i < info->num_regions; i++)
		if (info->regions[i].region.memory_class == memory_class &&
		     info->regions[i].region.memory_instance == memory_instance)
			return info->regions[i].unallocated_size;

	return 0;
}
