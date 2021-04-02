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

struct intel_allocator *intel_allocator_random_create(int fd);

struct intel_allocator_random {
	uint64_t bias;
	uint32_t prng;
	uint64_t gtt_size;
	uint64_t start;
	uint64_t end;

	/* statistics */
	uint64_t allocated_objects;
};

static uint64_t get_bias(int fd)
{
	(void) fd;

	return 256 << 10;
}

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

	(void) handle;
	(void) strategy;

	/* randomize the address, we try to avoid relocations */
	do {
		offset = hars_petruska_f54_1_random64(&ialr->prng);
		offset += ialr->bias; /* Keep the low 256k clear, for negative deltas */
		offset &= ialr->gtt_size - 1;
		offset &= ~(alignment - 1);
	} while (offset + size > ialr->end);

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

#define RESERVED 4096
struct intel_allocator *intel_allocator_random_create(int fd)
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
	ialr->gtt_size = gem_aperture_size(fd);
	igt_debug("Gtt size: %" PRId64 "\n", ialr->gtt_size);
	if (!gem_uses_full_ppgtt(fd))
		ialr->gtt_size /= 2;

	ialr->bias = get_bias(fd);
	ialr->start = ialr->bias;
	ialr->end = ialr->gtt_size - RESERVED;

	ialr->allocated_objects = 0;

	return ial;
}
