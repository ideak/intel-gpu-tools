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
#include "igt_map.h"

struct intel_allocator *
intel_allocator_reloc_create(int fd, uint64_t start, uint64_t end);

struct intel_allocator_reloc {
	struct igt_map *objects;
	uint32_t prng;
	uint64_t start;
	uint64_t end;
	uint64_t offset;

	/* statistics */
	uint64_t allocated_objects;
};

struct intel_allocator_record {
	uint32_t handle;
	uint64_t offset;
	uint64_t size;
};

/* Keep the low 256k clear, for negative deltas */
#define BIAS (256 << 10)

/* 2^31 + 2^29 - 2^25 + 2^22 - 2^19 - 2^16 + 1 */
#define GOLDEN_RATIO_PRIME_32 0x9e370001UL

/*  2^63 + 2^61 - 2^57 + 2^54 - 2^51 - 2^18 + 1 */
#define GOLDEN_RATIO_PRIME_64 0x9e37fffffffc0001ULL

static inline uint32_t hash_handles(const void *val)
{
	uint32_t hash = *(uint32_t *) val;

	hash = hash * GOLDEN_RATIO_PRIME_32;
	return hash;
}

static int equal_handles(const void *a, const void *b)
{
	uint32_t *key1 = (uint32_t *) a, *key2 = (uint32_t *) b;

	return *key1 == *key2;
}

static void map_entry_free_func(struct igt_map_entry *entry)
{
	free(entry->data);
}

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
	struct intel_allocator_record *rec;
	struct intel_allocator_reloc *ialr = ial->priv;
	uint64_t offset, aligned_offset;

	(void) strategy;

	rec = igt_map_search(ialr->objects, &handle);
	if (rec) {
		offset = rec->offset;
		igt_assert(rec->size == size);
	} else {
		aligned_offset = ALIGN(ialr->offset, alignment);

		/* Check we won't exceed end */
		if (aligned_offset + size > ialr->end)
			aligned_offset = ALIGN(ialr->start, alignment);

		/* Check that the object fits in the address range */
		if (aligned_offset + size > ialr->end)
			return ALLOC_INVALID_ADDRESS;

		offset = aligned_offset;

		rec = malloc(sizeof(*rec));
		rec->handle = handle;
		rec->offset = offset;
		rec->size = size;

		igt_map_insert(ialr->objects, &rec->handle, rec);

		ialr->offset = offset + size;
		ialr->allocated_objects++;
	}

	return offset;
}

static bool intel_allocator_reloc_free(struct intel_allocator *ial,
				       uint32_t handle)
{
	struct intel_allocator_record *rec = NULL;
	struct intel_allocator_reloc *ialr = ial->priv;
	struct igt_map_entry *entry;

	entry = igt_map_search_entry(ialr->objects, &handle);
	if (entry) {
		igt_map_remove_entry(ialr->objects, entry);
		if (entry->data) {
			rec = (struct intel_allocator_record *) entry->data;
			ialr->allocated_objects--;
			free(rec);

			return true;
		}
	}

	return false;
}

static inline bool __same(const struct intel_allocator_record *rec,
			  uint32_t handle, uint64_t size, uint64_t offset)
{
	return rec->handle == handle && rec->size == size &&
			DECANONICAL(rec->offset) == DECANONICAL(offset);
}

static bool intel_allocator_reloc_is_allocated(struct intel_allocator *ial,
					       uint32_t handle, uint64_t size,
					       uint64_t offset)
{
	struct intel_allocator_record *rec;
	struct intel_allocator_reloc *ialr;
	bool same = false;

	igt_assert(ial);
	ialr = (struct intel_allocator_reloc *) ial->priv;
	igt_assert(ialr);
	igt_assert(handle);

	rec = igt_map_search(ialr->objects, &handle);
	if (rec && __same(rec, handle, size, offset))
		same = true;

	return same;
}

static void intel_allocator_reloc_destroy(struct intel_allocator *ial)
{
	struct intel_allocator_reloc *ialr;

	igt_assert(ial);
	ialr = (struct intel_allocator_reloc *) ial->priv;

	igt_map_destroy(ialr->objects, map_entry_free_func);

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
	ialr->objects = igt_map_create(hash_handles, equal_handles);
	ialr->prng = (uint32_t) to_user_pointer(ial);

	start = max_t(uint64_t, start, BIAS);
	igt_assert(start < end);
	ialr->offset = ialr->start = start;
	ialr->end = end;

	ialr->allocated_objects = 0;

	return ial;
}
