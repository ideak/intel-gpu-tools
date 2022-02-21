// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <errno.h>
#include <pthread.h>

#include "drmtest.h"
#include "gem_create.h"
#include "i915_drm.h"
#include "igt_core.h"
#include "igt_list.h"
#include "igt_map.h"
#include "ioctl_wrappers.h"

/**
 * SECTION:gem_create
 * @short_description: Helpers for dealing with objects creation
 * @title: GEM Create
 *
 * This helper library contains functions used for handling creating gem
 * objects.
 */

int __gem_create(int fd, uint64_t *size, uint32_t *handle)
{
	struct drm_i915_gem_create create = {
		.size = *size,
	};
	int err = 0;

	if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_CREATE, &create) == 0) {
		*handle = create.handle;
		*size = create.size;
	} else {
		err = -errno;
		igt_assume(err != 0);
	}

	errno = 0;
	return err;
}

/**
 * gem_create:
 * @fd: open i915 drm file descriptor
 * @size: desired size of the buffer
 *
 * This wraps the GEM_CREATE ioctl, which allocates a new gem buffer object of
 * @size.
 *
 * Returns: The file-private handle of the created buffer object
 */
uint32_t gem_create(int fd, uint64_t size)
{
	uint32_t handle;

	igt_assert_eq(__gem_create(fd, &size, &handle), 0);

	return handle;
}

int __gem_create_ext(int fd, uint64_t *size, uint32_t flags, uint32_t *handle,
		     struct i915_user_extension *ext)
{
	struct drm_i915_gem_create_ext create = {
		.size = *size,
		.flags = flags,
		.extensions = to_user_pointer(ext),
	};
	int err = 0;

	if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_CREATE_EXT, &create) == 0) {
		*handle = create.handle;
		*size = create.size;
	} else {
		err = -errno;
		igt_assume(err != 0);
	}

	errno = 0;
	return err;
}

/**
 * gem_create_ext:
 * @fd: open i915 drm file descriptor
 * @size: desired size of the buffer
 * @flags: optional flags
 * @ext: optional extensions chain
 *
 * This wraps the GEM_CREATE_EXT ioctl, which allocates a new gem buffer object
 * of @size.
 *
 * Returns: The file-private handle of the created buffer object
 */
uint32_t gem_create_ext(int fd, uint64_t size, uint32_t flags,
			struct i915_user_extension *ext)
{
	uint32_t handle;

	igt_assert_eq(__gem_create_ext(fd, &size, flags, &handle, ext), 0);

	return handle;
}

static struct igt_map *pool;
static pthread_mutex_t pool_mutex = PTHREAD_MUTEX_INITIALIZER;

struct pool_entry {
	int fd;
	uint32_t handle;
	uint64_t size;		/* requested bo size */
	uint64_t bo_size;	/* created bo size */
	uint32_t region;
	struct igt_list_head link;
};

struct pool_list {
	uint64_t size;
	struct igt_list_head list;
};

static struct pool_entry *find_or_create(int fd, struct pool_list *pl,
					 uint64_t size, uint32_t region)
{
	struct pool_entry *pe;
	bool found = false;

	igt_list_for_each_entry(pe, &pl->list, link) {
		if (pe->fd == fd && pe->size == size && pe->region == region &&
		    !gem_bo_busy(fd, pe->handle)) {
			found = true;
			break;
		}
	}

	if (!found) {
		pe = calloc(1, sizeof(*pe));
		if (!pe)
			goto out;

		pe->fd = fd;
		pe->bo_size = size;
		if (__gem_create_in_memory_regions(fd, &pe->handle, &pe->bo_size, region)) {
			free(pe);
			pe = NULL;
			goto out;
		}
		pe->size = size;
		pe->region = region;

		igt_list_add_tail(&pe->link, &pl->list);
	}

out:
	return pe;
}

/**
 * gem_create_from_pool:
 * @fd: open i915 drm file descriptor
 * @size: pointer to size, on input it points to requested bo size,
 * on output created bo size will be stored there
 * @region: region in which bo should be created
 *
 * Function returns bo handle which is free to use (not busy). Internally
 * it iterates over previously allocated bo and returns first free. If there
 * are no free bo a new one is created.
 *
 * Returns: bo handle + created bo size (via pointer to size)
 */
uint32_t gem_create_from_pool(int fd, uint64_t *size, uint32_t region)
{
	struct pool_list *pl;
	struct pool_entry *pe;

	pthread_mutex_lock(&pool_mutex);

	pl = igt_map_search(pool, size);
	if (!pl) {
		pl = calloc(1, sizeof(*pl));
		if (!pl)
			goto out;

		IGT_INIT_LIST_HEAD(&pl->list);
		pl->size = *size;
		igt_map_insert(pool, &pl->size, pl);
	}
	pe = find_or_create(fd, pl, *size, region);

out:
	pthread_mutex_unlock(&pool_mutex);

	igt_assert(pl && pe);

	return pe->handle;
}

static void __pool_list_free_func(struct igt_map_entry *entry)
{
	free(entry->data);
}

static void __destroy_pool(struct igt_map *map, pthread_mutex_t *mutex)
{
	struct igt_map_entry *pos;
	const struct pool_list *pl;
	struct pool_entry *pe, *tmp;

	if (!map)
		return;

	pthread_mutex_lock(mutex);

	igt_map_foreach(map, pos) {
		pl = pos->key;
		igt_list_for_each_entry_safe(pe, tmp, &pl->list, link) {
			gem_close(pe->fd, pe->handle);
			igt_list_del(&pe->link);
			free(pe);
		}
	}

	pthread_mutex_unlock(mutex);

	igt_map_destroy(map, __pool_list_free_func);
}

void gem_pool_dump(void)
{
	struct igt_map_entry *pos;
	const struct pool_list *pl;
	struct pool_entry *pe;

	if (!pool)
		return;

	pthread_mutex_lock(&pool_mutex);

	igt_debug("[pool]\n");
	igt_map_foreach(pool, pos) {
		pl = pos->key;
		igt_debug("bucket [%llx]\n", (long long) pl->size);
		igt_list_for_each_entry(pe, &pl->list, link)
			igt_debug(" - handle: %u, size: %llx, bo_size: %llx, region: %x\n",
				  pe->handle, (long long) pe->size,
				  (long long) pe->bo_size, pe->region);
	}

	pthread_mutex_unlock(&pool_mutex);
}

#define GOLDEN_RATIO_PRIME_64 0x9e37fffffffc0001ULL
static inline uint32_t hash_pool(const void *val)
{
	uint64_t hash = *(uint64_t *) val;

	hash = hash * GOLDEN_RATIO_PRIME_64;
	return hash >> 32;
}

static int equal_pool(const void *a, const void *b)
{
	struct pool_list *p1 = (struct pool_list *) a;
	struct pool_list *p2 = (struct pool_list *) b;

	return p1->size == p2->size;
}

/**
 * gem_pool_init:
 *
 * Function initializes bo pool (kind of bo cache). Main purpose of it is to
 * support working with softpin to achieve pipelined execution on gpu (without
 * stalls).
 *
 * For example imagine code as follows:
 *
 * |[<!-- language="C" -->
 * uint32_t bb = gem_create(fd, 4096);
 * uint32_t *bbptr = gem_mmap__device_coherent(fd, bb, ...)
 * uint32_t *cmd = bbptr;
 * ...
 * *cmd++ = ...gpu commands...
 * ...
 * *cmd++ = MI_BATCH_BUFFER_END;
 * ...
 * gem_execbuf(fd, execbuf); // bb is part of execbuf   <--- first execbuf
 *
 * cmd = bbptr;
 * ...
 * *cmd++ = ... next gpu commands...
 * ...
 * *cmd++ = MI_BATCH_BUFFER_END;
 * ...
 * gem_execbuf(fd, execbuf); // bb is part of execbuf   <--- second execbuf
 * ]|
 *
 * Above code is prone to gpu hang because when bb was submitted to gpu
 * we immediately started writing to it. If gpu started executing commands
 * from first execbuf we're overwriting it leading to unpredicted behavior
 * (partially execute from first and second commands or we get gpu hang).
 * To avoid this we can sync after first execbuf but we will get stall
 * in execution. For some tests it might be accepted but such "isolated"
 * execution hides bugs (synchronization, cache flushes, etc).
 *
 * So, to achive pipelined execution we need to use another bb. If we would
 * like to enqueue more work which is serialized we would need more bbs
 * (depends on execution speed). Handling this manually is cumbersome as
 * we need to track all bb and their status (busy or free).
 *
 * Solution to above is gem pool. It returns first handle of requested size
 * which is not busy (or create a new one if there's none or all of bo are
 * in use). Here's an example how to use it:
 *
 * |[<!-- language="C" -->
 * uint64_t bbsize = 4096;
 * uint32_t bb = gem_create_from_pool(fd, &bbsize, REGION_SMEM);
 * uint32_t *bbptr = gem_mmap__device_coherent(fd, bb, ...)
 * uint32_t *cmd = bbptr;
 * ...
 * *cmd++ = ...gpu commands...
 * ...
 * *cmd++ = MI_BATCH_BUFFER_END;
 * gem_munmap(bbptr, bbsize);
 * ...
 * gem_execbuf(fd, execbuf); // bb is part of execbuf   <--- first execbuf
 *
 * bbsize = 4096;
 * bb = gem_create_from_pool(fd, &bbsize, REGION_SMEM);
 * cmd = bbptr;
 * ...
 * *cmd++ = ... next gpu commands...
 * ...
 * *cmd++ = MI_BATCH_BUFFER_END;
 * gem_munmap(bbptr, bbsize);
 * ...
 * gem_execbuf(fd, execbuf); // bb is part of execbuf   <--- second execbuf
 * ]|
 *
 * Assuming first execbuf is executed we will get new bb handle when we call
 * gem_create_from_pool(). When test completes pool is freed automatically
 * in igt core (all handles will be closed, memory will be freed and gem pool
 * will be reinitialized for next test).
 *
 * Some explanation is needed why we need to put pointer to size instead of
 * passing absolute value. On discrete regarding memory placement (region)
 * object created in the memory can be bigger than requested. Especially when
 * we use allocator to handle vm space and we allocate vma with requested
 * size (which is smaller than bo created) we can overlap with next allocation
 * and get -ENOSPC.
 */
void gem_pool_init(void)
{
	pthread_mutex_init(&pool_mutex, NULL);
	__destroy_pool(pool, &pool_mutex);
	pool = igt_map_create(hash_pool, equal_pool);
}

igt_constructor {
	gem_pool_init();
}
