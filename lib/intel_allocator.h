// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef __INTEL_ALLOCATOR_H__
#define __INTEL_ALLOCATOR_H__

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>

/**
 * SECTION:intel_allocator
 * @short_description: igt implementation of allocator
 * @title: Intel allocator
 * @include: intel_allocator.h
 *
 * # Introduction
 *
 * With the era of discrete cards we requested to adopt IGT to handle
 * addresses in userspace only (softpin, without support of relocations).
 * Writing an allocator for single purpose would be relatively easy
 * but supporting different tests with different requirements became
 * quite complicated task where couple of scenarios may be not covered yet.
 *
 * # Assumptions
 *
 * - Allocator has to work in multiprocess / multithread environment.
 * - Allocator backend (algorithm) should be plugable. Currently we support
 *   SIMPLE (borrowed from Mesa allocator), RELOC (pseudo allocator which
 *   returns incremented addresses without checking overlapping)
 *   and RANDOM (pseudo allocator which randomize addresses without
 *   checking overlapping).
 * - Has to integrate in intel-bb (our simpler libdrm replacement used in
 *   couple of tests).
 *
 * # Implementation
 *
 * ## Single process (allows multiple threads)
 *
 * For single process we don't need to create dedicated
 * entity (kind of arbiter) to solve allocations. Simple locking over
 * allocator data structure is enough. As basic usage example would be:
 *
 * |[<!-- language="c" -->
 * struct object {
 *      uint32_t handle;
 *      uint64_t offset;
 *      uint64_t size;
 * };
 *
 * struct object obj1, obj2;
 * uint64_t ahnd, startp, endp, size = 4096, align = 1 << 13;
 * int fd = -1;
 *
 * fd = drm_open_driver(DRIVER_INTEL);
 * ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_SIMPLE);
 *
 * obj1.handle = gem_create(4096);
 * obj2.handle = gem_create(4096);
 *
 * // Reserve hole for an object in given address.
 * // In this example the first possible address.
 * intel_allocator_get_address_range(ahnd, &startp, &endp);
 * obj1.offset = startp;
 * igt_assert(intel_allocator_reserve(ahnd, obj1.handle, size, startp));
 *
 * // Get the most suitable offset for the object. Preferred way.
 * obj2.offset = intel_allocator_alloc(ahnd, obj2.handle, size, align);
 *
 *  ...
 *
 * // Reserved addresses can be only freed by unreserve.
 * intel_allocator_unreserve(ahnd, obj1.handle, size, obj1.offset);
 * intel_allocator_free(ahnd, obj2.handle);
 *
 * gem_close(obj1.handle);
 * gem_close(obj2.handle);
 * ]|
 *
 * Description:
 * - ahnd is allocator handle (vm space handled by it)
 * - we call get_address_range() to get start/end range provided by the
 *   allocator (we haven't specified its range in open so allocator code will
 *   assume some safe address range - we don't want to exercise some potential
 *   HW bugs on the last page)
 * - alloc() / free() pair just gets address for gem object proposed by the
 *   allocator
 * - reserve() / unreserve() pair gives us full control of acquire/return
 *   range we're interested in
 *
 * ## Multiple processes
 *
 * When process forks and its child uses same fd vm its address space is also
 * the same. Some coordination - in this case interprocess communication -
 * is required to assign proper addresses for gem objects and avoid collision.
 * Additional thread is spawned for such case to cover child processes needs.
 * It uses some form of communication channel to receive, perform action
 * (alloc, free...) and send response to requesting process. Currently
 * SYSVIPC message queue was chosen for this but it can replaced by other
 * mechanism. Allocation techniques are same as for single process, we
 * just need to wrap such code with:
 *
 *
 * |[<!-- language="c" -->
 *
 *
 * intel_allocator_multiprocess_start();
 *
 * ... allocation code (open, close, alloc, free, ...)
 *
 * intel_allocator_multiprocess_stop();
 * ]|
 *
 * Calling start() spawns additional allocator thread ready for handling
 * incoming allocation requests (open / close are also requests in that case).
 *
 * Calling stop() request to stop allocator thread unblocking all pending
 * children (if any).
 */

enum allocator_strategy {
	ALLOC_STRATEGY_NONE,
	ALLOC_STRATEGY_LOW_TO_HIGH,
	ALLOC_STRATEGY_HIGH_TO_LOW
};

struct intel_allocator {
	int fd;
	uint8_t type;
	enum allocator_strategy strategy;
	_Atomic(int32_t) refcount;
	pthread_mutex_t mutex;

	/* allocator's private structure */
	void *priv;

	void (*get_address_range)(struct intel_allocator *ial,
				  uint64_t *startp, uint64_t *endp);
	uint64_t (*alloc)(struct intel_allocator *ial, uint32_t handle,
			  uint64_t size, uint64_t alignment);
	bool (*is_allocated)(struct intel_allocator *ial, uint32_t handle,
			     uint64_t size, uint64_t alignment);
	bool (*reserve)(struct intel_allocator *ial,
			uint32_t handle, uint64_t start, uint64_t size);
	bool (*unreserve)(struct intel_allocator *ial,
			  uint32_t handle, uint64_t start, uint64_t size);
	bool (*is_reserved)(struct intel_allocator *ial,
			    uint64_t start, uint64_t size);
	bool (*free)(struct intel_allocator *ial, uint32_t handle);

	void (*destroy)(struct intel_allocator *ial);

	bool (*is_empty)(struct intel_allocator *ial);

	void (*print)(struct intel_allocator *ial, bool full);
};

void intel_allocator_init(void);
void __intel_allocator_multiprocess_prepare(void);
void __intel_allocator_multiprocess_start(void);
void intel_allocator_multiprocess_start(void);
void intel_allocator_multiprocess_stop(void);

uint64_t intel_allocator_open(int fd, uint32_t ctx, uint8_t allocator_type);
uint64_t intel_allocator_open_full(int fd, uint32_t ctx,
				   uint64_t start, uint64_t end,
				   uint8_t allocator_type,
				   enum allocator_strategy strategy);
uint64_t intel_allocator_open_vm(int fd, uint32_t vm, uint8_t allocator_type);
uint64_t intel_allocator_open_vm_full(int fd, uint32_t vm,
				      uint64_t start, uint64_t end,
				      uint8_t allocator_type,
				      enum allocator_strategy strategy);

uint64_t intel_allocator_open_vm_as(uint64_t allocator_handle, uint32_t new_vm);
bool intel_allocator_close(uint64_t allocator_handle);
void intel_allocator_get_address_range(uint64_t allocator_handle,
				       uint64_t *startp, uint64_t *endp);
uint64_t __intel_allocator_alloc(uint64_t allocator_handle, uint32_t handle,
				 uint64_t size, uint64_t alignment);
uint64_t intel_allocator_alloc(uint64_t allocator_handle, uint32_t handle,
			       uint64_t size, uint64_t alignment);
bool intel_allocator_free(uint64_t allocator_handle, uint32_t handle);
bool intel_allocator_is_allocated(uint64_t allocator_handle, uint32_t handle,
				  uint64_t size, uint64_t offset);
bool intel_allocator_reserve(uint64_t allocator_handle, uint32_t handle,
			     uint64_t size, uint64_t offset);
bool intel_allocator_unreserve(uint64_t allocator_handle, uint32_t handle,
			       uint64_t size, uint64_t offset);
bool intel_allocator_is_reserved(uint64_t allocator_handle,
				 uint64_t size, uint64_t offset);
bool intel_allocator_reserve_if_not_allocated(uint64_t allocator_handle,
					      uint32_t handle,
					      uint64_t size, uint64_t offset,
					      bool *is_allocatedp);

void intel_allocator_print(uint64_t allocator_handle);

#define ALLOC_INVALID_ADDRESS (-1ull)
#define INTEL_ALLOCATOR_NONE   0
#define INTEL_ALLOCATOR_RELOC  1
#define INTEL_ALLOCATOR_RANDOM 2
#define INTEL_ALLOCATOR_SIMPLE 3

#define GEN8_GTT_ADDRESS_WIDTH 48

static inline uint64_t sign_extend64(uint64_t x, int high)
{
	int shift = 63 - high;

	return (int64_t)(x << shift) >> shift;
}

static inline uint64_t CANONICAL(uint64_t offset)
{
	return sign_extend64(offset, GEN8_GTT_ADDRESS_WIDTH - 1);
}

#define DECANONICAL(offset) (offset & ((1ull << GEN8_GTT_ADDRESS_WIDTH) - 1))

#endif
