/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 *
 * Authors:
 *    Jason Ekstrand <jason@jlekstrand.net>
 *    Maarten Lankhorst <maarten.lankhorst@linux.intel.com>
 *    Matthew Brost <matthew.brost@intel.com>
 */

#ifndef XE_IOCTL_H
#define XE_IOCTL_H

#include <stddef.h>
#include <stdint.h>
#include <xe_drm.h>

uint32_t xe_cs_prefetch_size(int fd);
uint32_t xe_vm_create(int fd, uint32_t flags, uint64_t ext);
int  __xe_vm_bind(int fd, uint32_t vm, uint32_t engine, uint32_t bo,
		  uint64_t offset, uint64_t addr, uint64_t size, uint32_t op,
		  struct drm_xe_sync *sync, uint32_t num_syncs, uint32_t region,
		  uint64_t ext);
void  __xe_vm_bind_assert(int fd, uint32_t vm, uint32_t engine, uint32_t bo,
			  uint64_t offset, uint64_t addr, uint64_t size,
			  uint32_t op, struct drm_xe_sync *sync,
			  uint32_t num_syncs, uint32_t region, uint64_t ext);
void xe_vm_bind(int fd, uint32_t vm, uint32_t bo, uint64_t offset,
		uint64_t addr, uint64_t size,
		struct drm_xe_sync *sync, uint32_t num_syncs);
void xe_vm_unbind(int fd, uint32_t vm, uint64_t offset,
		  uint64_t addr, uint64_t size,
		  struct drm_xe_sync *sync, uint32_t num_syncs);
void xe_vm_prefetch_async(int fd, uint32_t vm, uint32_t engine,
			  uint64_t offset, uint64_t addr, uint64_t size,
			  struct drm_xe_sync *sync, uint32_t num_syncs,
			  uint32_t region);
void xe_vm_bind_async(int fd, uint32_t vm, uint32_t engine, uint32_t bo,
		      uint64_t offset, uint64_t addr, uint64_t size,
		      struct drm_xe_sync *sync, uint32_t num_syncs);
void xe_vm_bind_userptr_async(int fd, uint32_t vm, uint32_t engine,
			      uint64_t userptr, uint64_t addr, uint64_t size,
			      struct drm_xe_sync *sync, uint32_t num_syncs);
void xe_vm_bind_async_flags(int fd, uint32_t vm, uint32_t engine, uint32_t bo,
			    uint64_t offset, uint64_t addr, uint64_t size,
			    struct drm_xe_sync *sync, uint32_t num_syncs,
			    uint32_t flags);
void xe_vm_bind_userptr_async_flags(int fd, uint32_t vm, uint32_t engine,
				    uint64_t userptr, uint64_t addr,
				    uint64_t size, struct drm_xe_sync *sync,
				    uint32_t num_syncs, uint32_t flags);
void xe_vm_unbind_async(int fd, uint32_t vm, uint32_t engine,
			uint64_t offset, uint64_t addr, uint64_t size,
			struct drm_xe_sync *sync, uint32_t num_syncs);
void xe_vm_bind_sync(int fd, uint32_t vm, uint32_t bo, uint64_t offset,
		     uint64_t addr, uint64_t size);
void xe_vm_unbind_sync(int fd, uint32_t vm, uint64_t offset,
		       uint64_t addr, uint64_t size);
void xe_vm_bind_array(int fd, uint32_t vm, uint32_t engine,
		      struct drm_xe_vm_bind_op *bind_ops,
		      uint32_t num_bind, struct drm_xe_sync *sync,
		      uint32_t num_syncs);
void xe_vm_unbind_all_async(int fd, uint32_t vm, uint32_t engine,
			    uint32_t bo, struct drm_xe_sync *sync,
			    uint32_t num_syncs);
void xe_vm_destroy(int fd, uint32_t vm);
uint32_t xe_bo_create_flags(int fd, uint32_t vm, uint64_t size, uint32_t flags);
uint32_t xe_bo_create(int fd, int gt, uint32_t vm, uint64_t size);
uint32_t xe_engine_create(int fd, uint32_t vm,
			  struct drm_xe_engine_class_instance *instance,
			  uint64_t ext);
uint32_t xe_bind_engine_create(int fd, uint32_t vm, uint64_t ext);
uint32_t xe_engine_create_class(int fd, uint32_t vm, uint16_t class);
void xe_engine_destroy(int fd, uint32_t engine);
uint64_t xe_bo_mmap_offset(int fd, uint32_t bo);
void *xe_bo_map(int fd, uint32_t bo, size_t size);
void *xe_bo_mmap_ext(int fd, uint32_t bo, size_t size, int prot);
void xe_exec(int fd, struct drm_xe_exec *exec);
void xe_exec_sync(int fd, uint32_t engine, uint64_t addr,
		  struct drm_xe_sync *sync, uint32_t num_syncs);
void xe_exec_wait(int fd, uint32_t engine, uint64_t addr);
void xe_wait_ufence(int fd, uint64_t *addr, uint64_t value,
		    struct drm_xe_engine_class_instance *eci,
		    int64_t timeout);
void xe_force_gt_reset(int fd, int gt);
void xe_vm_madvise(int fd, uint32_t vm, uint64_t addr, uint64_t size,
		   uint32_t property, uint32_t value);

#endif /* XE_IOCTL_H */
