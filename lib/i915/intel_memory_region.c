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

#ifdef __linux__
#include <linux/limits.h>
#endif
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>

#include "i915/gem_create.h"
#include "intel_reg.h"
#include "drmtest.h"
#include "ioctl_wrappers.h"
#include "igt_aux.h"
#include "igt_dummyload.h"
#include "igt_gt.h"
#include "igt_params.h"
#include "igt_sysfs.h"
#include "intel_chipset.h"
#include "igt_collection.h"
#include "igt_device.h"
#include "gem_mman.h"

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
	struct drm_i915_query_memory_regions *query_info = NULL;

	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_MEMORY_REGIONS;
	i915_query_items(fd, &item, 1);
	/*
	 * Any DRM_I915_QUERY_MEMORY_REGIONS specific errors are encoded in the
	 * item.length, even though the ioctl might still return success.
	 */

	if (item.length == -ENODEV) {
		/*
		 * If kernel supports query but not memory regions and it
		 * returns -ENODEV just return predefined system memory region
		 * only.
		 */
		size_t sys_qi_size = offsetof(typeof(*query_info), regions[1]);

		query_info = calloc(1, sys_qi_size);
		query_info->num_regions = 1;
		query_info->regions[0].region.memory_class = I915_MEMORY_CLASS_SYSTEM;
		goto out;
	} else if (item.length < 0) {
		/* Any other error are critial so no fallback is possible */
		igt_critical("DRM_I915_QUERY_MEMORY_REGIONS failed with %d\n",
			     item.length);
		goto out;
	}

	query_info = calloc(1, item.length);

	item.data_ptr = to_user_pointer(query_info);
	i915_query_items(fd, &item, 1);

out:
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
unsigned int gem_get_lmem_region_count(int fd)
{
	struct drm_i915_query_memory_regions *query_info;
	unsigned int lmem_regions = 0;

	query_info = gem_get_query_memory_regions(fd);
	if (!query_info)
		goto out;

	for (unsigned int i = 0; i < query_info->num_regions; i++) {
		if (query_info->regions[i].region.memory_class == I915_MEMORY_CLASS_DEVICE)
			lmem_regions += 1;
	}
	free(query_info);

out:
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
int __gem_create_in_memory_region_list(int fd, uint32_t *handle, uint64_t *size, uint32_t flags,
				       const struct drm_i915_gem_memory_class_instance *mem_regions,
				       int num_regions)
{
	struct drm_i915_gem_create_ext_memory_regions ext_regions = {
		.base = { .name = I915_GEM_CREATE_EXT_MEMORY_REGIONS },
		.num_regions = num_regions,
		.regions = to_user_pointer(mem_regions),
	};
	int ret;

	ret = __gem_create_ext(fd, size, flags, handle, &ext_regions.base);
	if (flags && ret == -EINVAL)
		ret = __gem_create_ext(fd, size, 0, handle, &ext_regions.base);

	/*
	 * Provide fallback for stable kernels if region passed in the array
	 * can be system memory. In this case we get -ENODEV but still
	 * we're able to allocate gem bo in system memory using legacy call.
	 */
	if (ret == -ENODEV)
		for (int i = 0; i < num_regions; i++)
			if (mem_regions[i].memory_class == I915_MEMORY_CLASS_SYSTEM) {
				ret = __gem_create(fd, size, handle);
				break;
			}

	return ret;
}

/* gem_create_in_memory_region_list:
 * @fd: opened i915 drm file descriptor
 * @size: requested size of the buffer
 * @mem_regions: memory regions array (priority list)
 * @num_regions: @mem_regions length
 */
uint32_t gem_create_in_memory_region_list(int fd, uint64_t size, uint32_t flags,
					  const struct drm_i915_gem_memory_class_instance *mem_regions,
					  int num_regions)
{
	uint32_t handle;
	int ret = __gem_create_in_memory_region_list(fd, &handle, &size, flags,
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

struct mmap_supported_region {
	uint32_t region;
	struct igt_list_head link;
};

/**
 * get_dma_buf_mmap_supported_set:
 * @i915: i915 drm file descriptor
 * @set: memory regions set
 *
 * Function constructs set with regions which supports dma-buf mapping.
 *
 * Returns: set of regions which allows do dma-buf mmap or NULL otherwise.
 *
 * Note: set (igt_collection) need to be destroyed after use.
 */
struct igt_collection *
get_dma_buf_mmap_supported_set(int i915, struct igt_collection *set)
{
	struct igt_collection *region, *supported_set = NULL;
	uint32_t reg;
	int dma_buf_fd;
	char *ptr;
	uint32_t handle, bosize = 4096;
	int count = 0;
	struct mmap_supported_region *mreg, *tmp;
	IGT_LIST_HEAD(region_list);

	for_each_combination(region, 1, set) {
		reg = igt_collection_get_value(region, 0);
		handle = gem_create_in_memory_regions(i915, bosize, reg);

		dma_buf_fd = prime_handle_to_fd(i915, handle);
		ptr = mmap(NULL, bosize, PROT_READ, MAP_SHARED, dma_buf_fd, 0);
		if (ptr != MAP_FAILED) {
			mreg = malloc(sizeof(*mreg));
			igt_assert(mreg);
			mreg->region = reg;
			igt_list_add_tail(&mreg->link, &region_list);
			count++;
		}
		munmap(ptr, bosize);
		gem_close(i915, handle);
		close(dma_buf_fd);
	}

	if (count) {
		int i = 0;

		supported_set = igt_collection_create(count);

		igt_list_for_each_entry_safe(mreg, tmp, &region_list, link) {
			igt_collection_set_value(supported_set, i++, mreg->region);
			igt_list_del(&mreg->link);
			free(mreg);
		}
	}

	return supported_set;
}

/**
 * intel_dump_gpu_meminfo:
 * @info: pointer to drm_i915_query_memory_regions structure
 *
 * Outputs memory regions and their sizes.
 */
void intel_dump_gpu_meminfo(const struct drm_i915_query_memory_regions *info)
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
uint32_t gpu_meminfo_region_count(const struct drm_i915_query_memory_regions *info,
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
uint64_t gpu_meminfo_region_total_size(const struct drm_i915_query_memory_regions *info,
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
uint64_t gpu_meminfo_region_total_available(const struct drm_i915_query_memory_regions *info,
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
uint64_t gpu_meminfo_region_size(const struct drm_i915_query_memory_regions *info,
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
uint64_t gpu_meminfo_region_available(const struct drm_i915_query_memory_regions *info,
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

#define PAGE_SIZE 4096

enum cache_entry_type {
	MIN_START_OFFSET,
	MIN_ALIGNMENT,
	SAFE_START_OFFSET,
	SAFE_ALIGNMENT,
};

struct cache_entry {
	uint16_t devid;
	enum cache_entry_type type;

	union {
		/* for MIN_START_OFFSET */
		struct {
			uint64_t offset;
			uint32_t region;
		} start;

		/* for MIN_ALIGNMENT */
		struct {
			uint64_t alignment;
			uint64_t region1;
			uint64_t region2;
		} minalign;

		/* for SAFE_START_OFFSET */
		uint64_t safe_start_offset;

		/* for SAFE_ALIGNMENT */
		uint64_t safe_alignment;
	};
	struct igt_list_head link;
};

static IGT_LIST_HEAD(cache);
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct cache_entry *find_entry_unlocked(enum cache_entry_type type,
					       uint16_t devid,
					       uint32_t region1,
					       uint32_t region2)
{
	struct cache_entry *entry;

	igt_list_for_each_entry(entry, &cache, link) {
		if (entry->type != type || entry->devid != devid)
			continue;

		switch (entry->type) {
		case MIN_START_OFFSET:
			if (entry->start.region == region1)
				return entry;
			continue;

		case MIN_ALIGNMENT:
			if (entry->minalign.region1 == region1 &&
			    entry->minalign.region2 == region2)
				return entry;
			continue;

		case SAFE_START_OFFSET:
		case SAFE_ALIGNMENT:
			return entry;
		}
	}

	return NULL;
}

/**
 * gem_detect_min_start_offset_for_region:
 * @i915: drm fd
 * @region: memory region
 *
 * Returns: minimum start offset at which kernel allows placing objects
 *          for memory region.
 */
uint64_t gem_detect_min_start_offset_for_region(int i915, uint32_t region)
{
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_execbuffer2 eb;
	uint64_t start_offset = 0;
	uint64_t bb_size = PAGE_SIZE;
	uint32_t *batch, ctx = 0;
	uint16_t devid = intel_get_drm_devid(i915);
	struct cache_entry *entry, *newentry;

	pthread_mutex_lock(&cache_mutex);
	entry = find_entry_unlocked(MIN_START_OFFSET, devid, region, 0);
	if (entry)
		goto out;
	pthread_mutex_unlock(&cache_mutex);

	/* Use separate context if possible to avoid offset overlapping */
	__gem_context_create(i915, &ctx);

	memset(&obj, 0, sizeof(obj));
	memset(&eb, 0, sizeof(eb));

	eb.buffers_ptr = to_user_pointer(&obj);
	eb.buffer_count = 1;
	eb.flags = I915_EXEC_DEFAULT;
	eb.rsvd1 = ctx;
	igt_assert(__gem_create_in_memory_regions(i915, &obj.handle, &bb_size, region) == 0);
	obj.flags = EXEC_OBJECT_PINNED;

	batch = gem_mmap__device_coherent(i915, obj.handle, 0, bb_size, PROT_WRITE);
	*batch = MI_BATCH_BUFFER_END;
	munmap(batch, bb_size);

	while (1) {
		obj.offset = start_offset;

		if (__gem_execbuf(i915, &eb) == 0)
			break;

		if (start_offset)
			start_offset <<= 1;
		else
			start_offset = PAGE_SIZE;

		if (start_offset >= 1ull << 32)
			obj.flags |= EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

		igt_assert(start_offset <= 1ull << 48);
	}
	gem_close(i915, obj.handle);
	if (ctx)
		gem_context_destroy(i915, ctx);

	newentry = malloc(sizeof(*newentry));
	if (!newentry)
		return start_offset;

	/* Check does other thread did the job before */
	pthread_mutex_lock(&cache_mutex);
	entry = find_entry_unlocked(MIN_START_OFFSET, devid, region, 0);
	if (entry)
		goto out;

	entry = newentry;
	entry->devid = devid;
	entry->type = MIN_START_OFFSET;
	entry->start.offset = start_offset;
	entry->start.region = region;
	igt_list_add(&entry->link, &cache);

out:
	pthread_mutex_unlock(&cache_mutex);

	return entry->start.offset;
}

/**
 * gem_detect_safe_start_offset:
 * @i915: drm fd
 *
 * Returns: finds start offset which can be used as first one regardless
 *          memory region. Useful if for some reason some regions don't allow
 *          starting from 0x0 offset.
 */
uint64_t gem_detect_safe_start_offset(int i915)
{
	struct drm_i915_query_memory_regions *query_info;
	struct igt_collection *regions, *set;
	uint32_t region;
	uint64_t offset = 0;
	uint16_t devid = intel_get_drm_devid(i915);
	struct cache_entry *entry, *newentry;

	pthread_mutex_lock(&cache_mutex);
	entry = find_entry_unlocked(SAFE_START_OFFSET, devid, 0, 0);
	if (entry)
		goto out;
	pthread_mutex_unlock(&cache_mutex);

	query_info = gem_get_query_memory_regions(i915);
	igt_assert(query_info);

	set = get_memory_region_set(query_info,
				    I915_SYSTEM_MEMORY,
				    I915_DEVICE_MEMORY);

	for_each_combination(regions, 1, set) {
		region = igt_collection_get_value(regions, 0);
		offset = max(offset,
			     gem_detect_min_start_offset_for_region(i915, region));
	}
	free(query_info);
	igt_collection_destroy(set);

	newentry = malloc(sizeof(*newentry));
	if (!newentry)
		return offset;

	pthread_mutex_lock(&cache_mutex);
	entry = find_entry_unlocked(SAFE_START_OFFSET, devid, 0, 0);
	if (entry)
		goto out;

	entry = newentry;
	entry->devid = devid;
	entry->type = SAFE_START_OFFSET;
	entry->safe_start_offset = offset;
	igt_list_add(&entry->link, &cache);

out:
	pthread_mutex_unlock(&cache_mutex);

	return entry->safe_start_offset;
}

/**
 * gem_detect_min_alignment_for_regions:
 * @i915: drm fd
 * @region1: first region
 * @region2: second region
 *
 * Returns: minimum alignment which must be used when objects from @region1 and
 * @region2 are going to interact.
 */
uint64_t gem_detect_min_alignment_for_regions(int i915,
					      uint32_t region1,
					      uint32_t region2)
{
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_execbuffer2 eb;
	uint64_t min_alignment = PAGE_SIZE;
	uint64_t bb_size = PAGE_SIZE, obj_size = PAGE_SIZE;
	uint32_t *batch, ctx = 0;
	uint16_t devid = intel_get_drm_devid(i915);
	struct cache_entry *entry, *newentry;

	pthread_mutex_lock(&cache_mutex);
	entry = find_entry_unlocked(MIN_ALIGNMENT, devid, region1, region2);
	if (entry)
		goto out;
	pthread_mutex_unlock(&cache_mutex);

	/* Use separate context if possible to avoid offset overlapping */
	__gem_context_create(i915, &ctx);

	memset(obj, 0, sizeof(obj));
	memset(&eb, 0, sizeof(eb));

	/* Establish bb offset first */
	eb.buffers_ptr = to_user_pointer(obj);
	eb.buffer_count = ARRAY_SIZE(obj);
	eb.flags = I915_EXEC_BATCH_FIRST | I915_EXEC_DEFAULT;
	eb.rsvd1 = ctx;
	igt_assert(__gem_create_in_memory_regions(i915, &obj[0].handle,
						  &bb_size, region1) == 0);

	batch = gem_mmap__device_coherent(i915, obj[0].handle, 0, bb_size,
					  PROT_WRITE);
	*batch = MI_BATCH_BUFFER_END;
	munmap(batch, bb_size);

	obj[0].flags = EXEC_OBJECT_PINNED;
	obj[0].offset = gem_detect_min_start_offset_for_region(i915, region1);

	/* Find appropriate alignment of object */
	igt_assert(__gem_create_in_memory_regions(i915, &obj[1].handle,
						  &obj_size, region2) == 0);
	obj[1].handle = gem_create_in_memory_regions(i915, PAGE_SIZE, region2);
	obj[1].flags = EXEC_OBJECT_PINNED;
	while (1) {
		obj[1].offset = ALIGN(obj[0].offset + bb_size, min_alignment);
		igt_assert(obj[1].offset <= 1ull << 32);

		if (__gem_execbuf(i915, &eb) == 0)
			break;

		min_alignment <<= 1;
	}

	gem_close(i915, obj[0].handle);
	gem_close(i915, obj[1].handle);
	if (ctx)
		gem_context_destroy(i915, ctx);

	newentry = malloc(sizeof(*newentry));
	if (!newentry)
		return min_alignment;

	pthread_mutex_lock(&cache_mutex);
	entry = find_entry_unlocked(MIN_ALIGNMENT, devid, region1, region2);
	if (entry)
		goto out;

	entry = newentry;
	entry->devid = devid;
	entry->type = MIN_ALIGNMENT;
	entry->minalign.alignment = min_alignment;
	entry->minalign.region1 = region1;
	entry->minalign.region2 = region2;
	igt_list_add(&entry->link, &cache);

out:
	pthread_mutex_unlock(&cache_mutex);

	return entry->minalign.alignment;
}

/**
 * gem_detect_safe_alignment:
 * @i915: drm fd
 *
 * Returns: safe alignment for all memory regions on @i915 device.
 * Safe in this case means max() from all minimum alignments for each
 * region.
 */
uint64_t gem_detect_safe_alignment(int i915)
{
	struct drm_i915_query_memory_regions *query_info;
	struct igt_collection *regions, *set;
	uint64_t default_alignment = 0;
	uint32_t region_bb, region_obj;
	uint16_t devid = intel_get_drm_devid(i915);
	struct cache_entry *entry, *newentry;

	/* non-discrete uses 4K page size */
	if (!gem_has_lmem(i915))
		return PAGE_SIZE;

	pthread_mutex_lock(&cache_mutex);
	entry = find_entry_unlocked(SAFE_ALIGNMENT, devid, 0, 0);
	if (entry)
		goto out;
	pthread_mutex_unlock(&cache_mutex);

	query_info = gem_get_query_memory_regions(i915);
	igt_assert(query_info);

	set = get_memory_region_set(query_info,
				    I915_SYSTEM_MEMORY,
				    I915_DEVICE_MEMORY);

	for_each_variation_r(regions, 2, set) {
		uint64_t alignment;

		region_bb = igt_collection_get_value(regions, 0);
		region_obj = igt_collection_get_value(regions, 1);

		/* We're interested in triangular matrix */
		if (region_bb > region_obj)
			continue;

		alignment = gem_detect_min_alignment_for_regions(i915,
								 region_bb,
								 region_obj);
		if (default_alignment < alignment)
			default_alignment = alignment;
	}

	free(query_info);
	igt_collection_destroy(set);

	newentry = malloc(sizeof(*newentry));
	if (!newentry)
		return default_alignment;

	/* Try again, check does we have cache updated in the meantime. */
	pthread_mutex_lock(&cache_mutex);
	entry = find_entry_unlocked(SAFE_ALIGNMENT, devid,  0, 0);
	if (entry)
		goto out;

	entry = newentry;
	entry->devid = devid;
	entry->type = SAFE_ALIGNMENT;
	entry->safe_alignment = default_alignment;
	igt_list_add(&entry->link, &cache);

out:
	pthread_mutex_unlock(&cache_mutex);

	return entry->minalign.alignment;
}

static const char *
region_repr(const struct drm_i915_gem_memory_class_instance *ci)
{
	switch (ci->memory_class) {
	case I915_MEMORY_CLASS_SYSTEM:
		return "smem";
	case I915_MEMORY_CLASS_DEVICE:
		return "lmem";
	default:
		return "unknown";
	}
}

struct gem_memory_region *__gem_get_memory_regions(int i915)
{
	struct drm_i915_query_memory_regions *info;
	struct gem_memory_region *first = NULL;

	info = gem_get_query_memory_regions(i915);
	for (int i = 0; info && i < info->num_regions; i++) {
		struct gem_memory_region *r;

		r = malloc(sizeof(*r));
		igt_assert(r);

		r->ci = info->regions[i].region;
		r->size = info->regions[i].probed_size;
		r->cpu_size = info->regions[i].probed_cpu_visible_size;
		if (r->size == -1ull)
			r->size = igt_get_avail_ram_mb() << 20;

		asprintf(&r->name, "%s%d",
			 region_repr(&r->ci), r->ci.memory_instance);

		r->next = first;
		first = r;
	}
	free(info);

	return first;
}

struct gem_memory_region *__gem_next_memory_region(struct gem_memory_region *r)
{
	struct gem_memory_region *next = r->next;

	free(r->name);
	free(r);

	return next;
}
