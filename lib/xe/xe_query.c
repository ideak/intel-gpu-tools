// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 *
 * Authors:
 *    Matthew Brost <matthew.brost@intel.com>
 */

#include <stdlib.h>
#include <pthread.h>

#include "drmtest.h"
#include "ioctl_wrappers.h"
#include "igt_map.h"

#include "xe_query.h"
#include "xe_ioctl.h"

static struct drm_xe_query_config *xe_query_config_new(int fd)
{
	struct drm_xe_query_config *config;
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = DRM_XE_DEVICE_QUERY_CONFIG,
		.size = 0,
		.data = 0,
	};

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	config = malloc(query.size);
	igt_assert(config);

	query.data = to_user_pointer(config);
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	igt_assert(config->num_params > 0);

	return config;
}

static struct drm_xe_query_gts *xe_query_gts_new(int fd)
{
	struct drm_xe_query_gts *gts;
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = DRM_XE_DEVICE_QUERY_GTS,
		.size = 0,
		.data = 0,
	};

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	gts = malloc(query.size);
	igt_assert(gts);

	query.data = to_user_pointer(gts);
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	return gts;
}

static uint64_t __memory_regions(const struct drm_xe_query_gts *gts)
{
	uint64_t regions = 0;
	int i;

	for (i = 0; i < gts->num_gt; i++)
		regions |= gts->gts[i].native_mem_regions |
			   gts->gts[i].slow_mem_regions;

	return regions;
}

static struct drm_xe_engine_class_instance *
xe_query_engines_new(int fd, unsigned int *num_engines)
{
	struct drm_xe_engine_class_instance *hw_engines;
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = DRM_XE_DEVICE_QUERY_ENGINES,
		.size = 0,
		.data = 0,
	};

	igt_assert(num_engines);
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	hw_engines = malloc(query.size);
	igt_assert(hw_engines);

	query.data = to_user_pointer(hw_engines);
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	*num_engines = query.size / sizeof(*hw_engines);

	return hw_engines;
}

static struct drm_xe_query_mem_usage *xe_query_mem_usage_new(int fd)
{
	struct drm_xe_query_mem_usage *mem_usage;
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = DRM_XE_DEVICE_QUERY_MEM_USAGE,
		.size = 0,
		.data = 0,
	};

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	mem_usage = malloc(query.size);
	igt_assert(mem_usage);

	query.data = to_user_pointer(mem_usage);
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	return mem_usage;
}

static uint64_t native_region_for_gt(const struct drm_xe_query_gts *gts, int gt)
{
	uint64_t region;

	igt_assert(gts->num_gt > gt);
	region = gts->gts[gt].native_mem_regions;
	igt_assert(region);

	return region;
}

static uint64_t gt_vram_size(const struct drm_xe_query_mem_usage *mem_usage,
			     const struct drm_xe_query_gts *gts, int gt)
{
	int region_idx = ffs(native_region_for_gt(gts, gt)) - 1;

	if (XE_IS_CLASS_VRAM(&mem_usage->regions[region_idx]))
		return mem_usage->regions[region_idx].total_size;

	return 0;
}

static bool __mem_has_vram(struct drm_xe_query_mem_usage *mem_usage)
{
	for (int i = 0; i < mem_usage->num_regions; i++)
		if (XE_IS_CLASS_VRAM(&mem_usage->regions[i]))
			return true;

	return false;
}

static uint32_t __mem_default_alignment(struct drm_xe_query_mem_usage *mem_usage)
{
	uint32_t alignment = XE_DEFAULT_ALIGNMENT;

	for (int i = 0; i < mem_usage->num_regions; i++)
		if (alignment < mem_usage->regions[i].min_page_size)
			alignment = mem_usage->regions[i].min_page_size;

	return alignment;
}

/**
 * xe_engine_class_string:
 * @engine_class: engine class
 *
 * Returns engine class name or 'unknown class engine' otherwise.
 */
const char *xe_engine_class_string(uint32_t engine_class)
{
	switch (engine_class) {
		case DRM_XE_ENGINE_CLASS_RENDER:
			return "DRM_XE_ENGINE_CLASS_RENDER";
		case DRM_XE_ENGINE_CLASS_COPY:
			return "DRM_XE_ENGINE_CLASS_COPY";
		case DRM_XE_ENGINE_CLASS_VIDEO_DECODE:
			return "DRM_XE_ENGINE_CLASS_VIDEO_DECODE";
		case DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE:
			return "DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE";
		case DRM_XE_ENGINE_CLASS_COMPUTE:
			return "DRM_XE_ENGINE_CLASS_COMPUTE";
		default:
			igt_warn("Engine class 0x%x unknown\n", engine_class);
			return "unknown engine class";
	}
}

static struct xe_device_cache {
	pthread_mutex_t cache_mutex;
	struct igt_map *map;
} cache;

static struct xe_device *find_in_cache_unlocked(int fd)
{
	return igt_map_search(cache.map, &fd);
}

static struct xe_device *find_in_cache(int fd)
{
	struct xe_device *xe_dev;

	pthread_mutex_lock(&cache.cache_mutex);
	xe_dev = find_in_cache_unlocked(fd);
	pthread_mutex_unlock(&cache.cache_mutex);

	return xe_dev;
}

static void xe_device_free(struct xe_device *xe_dev)
{
	free(xe_dev->config);
	free(xe_dev->gts);
	free(xe_dev->hw_engines);
	free(xe_dev->mem_usage);
	free(xe_dev->vram_size);
	free(xe_dev);
}

/**
 * xe_device_get:
 * @fd: xe device fd
 *
 * Function creates and caches xe_device struct which contains configuration
 * data returned in few queries. Subsequent calls returns previously
 * created xe_device. To remove this from cache xe_device_put() must be
 * called.
 */
struct xe_device *xe_device_get(int fd)
{
	struct xe_device *xe_dev, *prev;

	xe_dev = find_in_cache(fd);
	if (xe_dev)
		return xe_dev;

	xe_dev = calloc(1, sizeof(*xe_dev));
	igt_assert(xe_dev);

	xe_dev->fd = fd;
	xe_dev->config = xe_query_config_new(fd);
	xe_dev->number_gt = xe_dev->config->info[XE_QUERY_CONFIG_GT_COUNT];
	xe_dev->va_bits = xe_dev->config->info[XE_QUERY_CONFIG_VA_BITS];
	xe_dev->dev_id = xe_dev->config->info[XE_QUERY_CONFIG_REV_AND_DEVICE_ID] & 0xffff;
	xe_dev->gts = xe_query_gts_new(fd);
	xe_dev->memory_regions = __memory_regions(xe_dev->gts);
	xe_dev->hw_engines = xe_query_engines_new(fd, &xe_dev->number_hw_engines);
	xe_dev->mem_usage = xe_query_mem_usage_new(fd);
	xe_dev->vram_size = calloc(xe_dev->number_gt, sizeof(*xe_dev->vram_size));
	for (int gt = 0; gt < xe_dev->number_gt; gt++)
		xe_dev->vram_size[gt] = gt_vram_size(xe_dev->mem_usage,
						     xe_dev->gts, gt);
	xe_dev->default_alignment = __mem_default_alignment(xe_dev->mem_usage);
	xe_dev->has_vram = __mem_has_vram(xe_dev->mem_usage);

	/* We may get here from multiple threads, use first cached xe_dev */
	pthread_mutex_lock(&cache.cache_mutex);
	prev = find_in_cache_unlocked(fd);
	if (!prev) {
		igt_map_insert(cache.map, &xe_dev->fd, xe_dev);
	} else {
		xe_device_free(xe_dev);
		xe_dev = prev;
	}
	pthread_mutex_unlock(&cache.cache_mutex);

	return xe_dev;
}

static void delete_in_cache(struct igt_map_entry *entry)
{
	xe_device_free((struct xe_device *)entry->data);
}

/**
 * xe_device_put:
 * @fd: xe device fd
 *
 * Remove previously allocated and cached xe_device (if any).
 */
void xe_device_put(int fd)
{
	pthread_mutex_lock(&cache.cache_mutex);
	if (find_in_cache_unlocked(fd))
		igt_map_remove(cache.map, &fd, delete_in_cache);
	pthread_mutex_unlock(&cache.cache_mutex);
}

/**
 * xe_supports_faults:
 * @fd: xe device fd
 *
 * Returns true if xe device @fd allows creating vm in fault mode otherwise
 * false.
 *
 * NOTE: This function temporarily creates a VM in fault mode. Hence, while
 * this function is executing, no non-fault mode VMs can be created.
 */
bool xe_supports_faults(int fd)
{
	bool supports_faults;

	struct drm_xe_vm_create create = {
		.flags = DRM_XE_VM_CREATE_ASYNC_BIND_OPS |
			 DRM_XE_VM_CREATE_FAULT_MODE,
	};

	supports_faults = !igt_ioctl(fd, DRM_IOCTL_XE_VM_CREATE, &create);

	if (supports_faults)
		xe_vm_destroy(fd, create.vm_id);

	return supports_faults;
}

static void xe_device_destroy_cache(void)
{
	pthread_mutex_lock(&cache.cache_mutex);
	igt_map_destroy(cache.map, delete_in_cache);
	pthread_mutex_unlock(&cache.cache_mutex);
}

static void xe_device_cache_init(void)
{
	pthread_mutex_init(&cache.cache_mutex, NULL);
	xe_device_destroy_cache();
	cache.map = igt_map_create(igt_map_hash_32, igt_map_equal_32);
}

#define xe_dev_FN(_NAME, _FIELD, _TYPE) \
_TYPE _NAME(int fd)			\
{					\
	struct xe_device *xe_dev;	\
					\
	xe_dev = find_in_cache(fd);	\
	igt_assert(xe_dev);		\
	return xe_dev->_FIELD;		\
}

/**
 * xe_number_gt:
 * @fd: xe device fd
 *
 * Return number of gts for xe device fd.
 */
xe_dev_FN(xe_number_gt, number_gt, unsigned int);

/**
 * all_memory_regions:
 * @fd: xe device fd
 *
 * Returns memory regions bitmask for xe device @fd.
 */
xe_dev_FN(all_memory_regions, memory_regions, uint64_t);

/**
 * system_memory:
 * @fd: xe device fd
 *
 * Returns system memory bitmask for xe device @fd.
 */
uint64_t system_memory(int fd)
{
	uint64_t regions = all_memory_regions(fd);

	return regions & 0x1;
}

/**
 * vram_memory:
 * @fd: xe device fd
 * @gt: gt id
 *
 * Returns vram memory bitmask for xe device @fd and @gt id.
 */
uint64_t vram_memory(int fd, int gt)
{
	struct xe_device *xe_dev;

	xe_dev = find_in_cache(fd);
	igt_assert(xe_dev);
	igt_assert(gt >= 0 && gt < xe_dev->number_gt);

	return xe_has_vram(fd) ? native_region_for_gt(xe_dev->gts, gt) : 0;
}

/**
 * vram_if_possible:
 * @fd: xe device fd
 * @gt: gt id
 *
 * Returns vram memory bitmask for xe device @fd and @gt id or system memory
 * if there's no vram memory available for @gt.
 */
uint64_t vram_if_possible(int fd, int gt)
{
	return vram_memory(fd, gt) ?: system_memory(fd);
}

/**
 * xe_hw_engines:
 * @fd: xe device fd
 *
 * Returns engines array of xe device @fd.
 */
xe_dev_FN(xe_hw_engines, hw_engines, struct drm_xe_engine_class_instance *);

/**
 * xe_hw_engine:
 * @fd: xe device fd
 * @idx: engine index
 *
 * Returns engine instance of xe device @fd and @idx.
 */
struct drm_xe_engine_class_instance *xe_hw_engine(int fd, int idx)
{
	struct xe_device *xe_dev;

	xe_dev = find_in_cache(fd);
	igt_assert(xe_dev);
	igt_assert(idx >= 0 && idx < xe_dev->number_hw_engines);

	return &xe_dev->hw_engines[idx];
}

struct drm_xe_query_mem_region *xe_mem_region(int fd, uint64_t region)
{
	struct xe_device *xe_dev;
	int region_idx = ffs(region) - 1;

	xe_dev = find_in_cache(fd);
	igt_assert(xe_dev);
	igt_assert(xe_dev->mem_usage->num_regions > region_idx);

	return &xe_dev->mem_usage->regions[region_idx];
}

/**
 * xe_number_hw_engine:
 * @fd: xe device fd
 *
 * Returns number of hw engines of xe device @fd.
 */
xe_dev_FN(xe_number_hw_engines, number_hw_engines, unsigned int);

/**
 * xe_has_vram:
 * @fd: xe device fd
 *
 * Returns true if xe device @fd has vram otherwise false.
 */
xe_dev_FN(xe_has_vram, has_vram, bool);

/**
 * xe_vram_size:
 * @fd: xe device fd
 * @gt: gt
 *
 * Returns size of vram of xe device @fd.
 */
uint64_t xe_vram_size(int fd, int gt)
{
	struct xe_device *xe_dev;

	xe_dev = find_in_cache(fd);
	igt_assert(xe_dev);

	return xe_dev->vram_size[gt];
}

/**
 * xe_get_default_alignment:
 * @fd: xe device fd
 *
 * Returns default alignment of objects for xe device @fd.
 */
xe_dev_FN(xe_get_default_alignment, default_alignment, uint32_t);

/**
 * xe_va_bits:
 * @fd: xe device fd
 *
 * Returns number of virtual address bits used in xe device @fd.
 */
xe_dev_FN(xe_va_bits, va_bits, uint32_t);

/**
 * xe_dev_id:
 * @fd: xe device fd
 *
 * Returns Device id of xe device @fd.
 */
xe_dev_FN(xe_dev_id, dev_id, uint16_t);

igt_constructor
{
	xe_device_cache_init();
}
