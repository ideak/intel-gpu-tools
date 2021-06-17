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
intel_allocator_random_create(int fd, uint64_t start, uint64_t end);

struct intel_allocator_random {
	uint32_t prng;
	uint64_t start;
	uint64_t end;

	/* statistics */
	uint64_t allocated_objects;
};

/* Keep the low 256k clear, for negative deltas */
#define BIAS (256 << 10)
#define RETRIES 8

static void intel_allocator_random_get_address_range(struct intel_allocator *ial,
						     uint64_t *startp,
						     uint64_t *endp)
{
	struct intel_allocator_random *ialr = ial->priv;

	if (startp)
		*startp = ialr->start;

	if (endp)
		*endp = ialr->end;
}

static uint64_t intel_allocator_random_alloc(struct intel_allocator *ial,
					     uint32_t handle, uint64_t size,
					     uint64_t alignment,
					     enum allocator_strategy strategy)
{
	struct intel_allocator_random *ialr = ial->priv;
	uint64_t offset;
	int cnt = RETRIES;

	(void) handle;
	(void) strategy;

	/* randomize the address, we try to avoid relocations */
	do {
		offset = hars_petruska_f54_1_random64(&ialr->prng);
		/* maximize the chances of fitting in the last iteration */
		if (cnt == 1)
			offset = 0;

		offset %= ialr->end - ialr->start;
		offset += ialr->start;
		offset = ALIGN(offset, alignment);
	} while (offset + size > ialr->end && --cnt);

	if (!cnt)
		return ALLOC_INVALID_ADDRESS;

	ialr->allocated_objects++;

	return offset;
}

static bool intel_allocator_random_free(struct intel_allocator *ial,
					uint32_t handle)
{
	struct intel_allocator_random *ialr = ial->priv;

	(void) handle;

	ialr->allocated_objects--;

	return false;
}

static bool intel_allocator_random_is_allocated(struct intel_allocator *ial,
						uint32_t handle, uint64_t size,
						uint64_t offset)
{
	(void) ial;
	(void) handle;
	(void) size;
	(void) offset;

	return false;
}

static void intel_allocator_random_destroy(struct intel_allocator *ial)
{
	igt_assert(ial);

	free(ial->priv);
	free(ial);
}

static bool intel_allocator_random_reserve(struct intel_allocator *ial,
					   uint32_t handle,
					   uint64_t start, uint64_t end)
{
	(void) ial;
	(void) handle;
	(void) start;
	(void) end;

	return false;
}

static bool intel_allocator_random_unreserve(struct intel_allocator *ial,
					     uint32_t handle,
					     uint64_t start, uint64_t end)
{
	(void) ial;
	(void) handle;
	(void) start;
	(void) end;

	return false;
}

static bool intel_allocator_random_is_reserved(struct intel_allocator *ial,
					       uint64_t start, uint64_t end)
{
	(void) ial;
	(void) start;
	(void) end;

	return false;
}

static void intel_allocator_random_print(struct intel_allocator *ial, bool full)
{
	struct intel_allocator_random *ialr = ial->priv;

	(void) full;

	igt_info("<ial: %p, fd: %d> allocated objects: %" PRIx64 "\n",
		 ial, ial->fd, ialr->allocated_objects);
}

static bool intel_allocator_random_is_empty(struct intel_allocator *ial)
{
	struct intel_allocator_random *ialr = ial->priv;

	return !ialr->allocated_objects;
}

struct intel_allocator *
intel_allocator_random_create(int fd, uint64_t start, uint64_t end)
{
	struct intel_allocator *ial;
	struct intel_allocator_random *ialr;

	igt_debug("Using random allocator\n");
	ial = calloc(1, sizeof(*ial));
	igt_assert(ial);

	ial->fd = fd;
	ial->get_address_range = intel_allocator_random_get_address_range;
	ial->alloc = intel_allocator_random_alloc;
	ial->free = intel_allocator_random_free;
	ial->is_allocated = intel_allocator_random_is_allocated;
	ial->reserve = intel_allocator_random_reserve;
	ial->unreserve = intel_allocator_random_unreserve;
	ial->is_reserved = intel_allocator_random_is_reserved;
	ial->destroy = intel_allocator_random_destroy;
	ial->print = intel_allocator_random_print;
	ial->is_empty = intel_allocator_random_is_empty;

	ialr = ial->priv = calloc(1, sizeof(*ialr));
	igt_assert(ial->priv);
	ialr->prng = (uint32_t) to_user_pointer(ial);

	start = max(start, BIAS);
	igt_assert(start < end);
	ialr->start = start;
	ialr->end = end;

	ialr->allocated_objects = 0;

	return ial;
}
