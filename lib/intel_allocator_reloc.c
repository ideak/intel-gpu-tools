// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <sys/ioctl.h>
#include <stdlib.h>
#include "igt.h"
#include "igt_x86.h"
#include "igt_rand.h"
#include "intel_allocator.h"

struct intel_allocator *
intel_allocator_reloc_create(int fd, uint64_t start, uint64_t end);

struct intel_allocator_reloc {
	uint32_t prng;
	uint64_t start;
	uint64_t end;
	uint64_t offset;

	/* statistics */
	uint64_t allocated_objects;
};

/* Keep the low 256k clear, for negative deltas */
#define BIAS (256 << 10)

static void intel_allocator_reloc_get_address_range(struct intel_allocator *ial,
						    uint64_t *startp,
						    uint64_t *endp)
{
	struct intel_allocator_reloc *ialr = ial->priv;

	if (startp)
		*startp = ialr->start;

	if (endp)
		*endp = ialr->end;
}

static uint64_t intel_allocator_reloc_alloc(struct intel_allocator *ial,
					    uint32_t handle, uint64_t size,
					    uint64_t alignment,
					    enum allocator_strategy strategy)
{
	struct intel_allocator_reloc *ialr = ial->priv;
	uint64_t offset, aligned_offset;

	(void) handle;
	(void) strategy;

	aligned_offset = ALIGN(ialr->offset, alignment);

	/* Check we won't exceed end */
	if (aligned_offset + size > ialr->end)
		aligned_offset = ALIGN(ialr->start, alignment);

	/* Check that the object fits in the address range */
	if (aligned_offset + size > ialr->end)
		return ALLOC_INVALID_ADDRESS;

	offset = aligned_offset;
	ialr->offset = offset + size;
	ialr->allocated_objects++;

	return offset;
}

static bool intel_allocator_reloc_free(struct intel_allocator *ial,
				       uint32_t handle)
{
	struct intel_allocator_reloc *ialr = ial->priv;

	(void) handle;

	ialr->allocated_objects--;

	return false;
}

static bool intel_allocator_reloc_is_allocated(struct intel_allocator *ial,
					       uint32_t handle, uint64_t size,
					       uint64_t offset)
{
	(void) ial;
	(void) handle;
	(void) size;
	(void) offset;

	return false;
}

static void intel_allocator_reloc_destroy(struct intel_allocator *ial)
{
	igt_assert(ial);

	free(ial->priv);
	free(ial);
}

static bool intel_allocator_reloc_reserve(struct intel_allocator *ial,
					  uint32_t handle,
					  uint64_t start, uint64_t end)
{
	(void) ial;
	(void) handle;
	(void) start;
	(void) end;

	return false;
}

static bool intel_allocator_reloc_unreserve(struct intel_allocator *ial,
					    uint32_t handle,
					    uint64_t start, uint64_t end)
{
	(void) ial;
	(void) handle;
	(void) start;
	(void) end;

	return false;
}

static bool intel_allocator_reloc_is_reserved(struct intel_allocator *ial,
					      uint64_t start, uint64_t end)
{
	(void) ial;
	(void) start;
	(void) end;

	return false;
}

static void intel_allocator_reloc_print(struct intel_allocator *ial, bool full)
{
	struct intel_allocator_reloc *ialr = ial->priv;

	(void) full;

	igt_info("<ial: %p, fd: %d> allocated objects: %" PRIx64 "\n",
		 ial, ial->fd, ialr->allocated_objects);
}

static bool intel_allocator_reloc_is_empty(struct intel_allocator *ial)
{
	struct intel_allocator_reloc *ialr = ial->priv;

	return !ialr->allocated_objects;
}

struct intel_allocator *
intel_allocator_reloc_create(int fd, uint64_t start, uint64_t end)
{
	struct intel_allocator *ial;
	struct intel_allocator_reloc *ialr;

	igt_debug("Using reloc allocator\n");
	ial = calloc(1, sizeof(*ial));
	igt_assert(ial);

	ial->fd = fd;
	ial->get_address_range = intel_allocator_reloc_get_address_range;
	ial->alloc = intel_allocator_reloc_alloc;
	ial->free = intel_allocator_reloc_free;
	ial->is_allocated = intel_allocator_reloc_is_allocated;
	ial->reserve = intel_allocator_reloc_reserve;
	ial->unreserve = intel_allocator_reloc_unreserve;
	ial->is_reserved = intel_allocator_reloc_is_reserved;
	ial->destroy = intel_allocator_reloc_destroy;
	ial->print = intel_allocator_reloc_print;
	ial->is_empty = intel_allocator_reloc_is_empty;

	ialr = ial->priv = calloc(1, sizeof(*ialr));
	igt_assert(ial->priv);
	ialr->prng = (uint32_t) to_user_pointer(ial);

	start = max(start, BIAS);
	igt_assert(start < end);
	ialr->offset = ialr->start = start;
	ialr->end = end;

	ialr->allocated_objects = 0;

	return ial;
}
