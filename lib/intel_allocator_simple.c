// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <sys/ioctl.h>
#include <stdlib.h>
#include "igt.h"
#include "igt_x86.h"
#include "intel_allocator.h"
#include "intel_bufops.h"
#include "igt_map.h"


/* Avoid compilation warning */
struct intel_allocator *
intel_allocator_simple_create(int fd, uint64_t start, uint64_t end,
			      enum allocator_strategy strategy);

struct simple_vma_heap {
	struct igt_list_head holes;
	enum allocator_strategy strategy;
};

struct simple_vma_hole {
	struct igt_list_head link;
	uint64_t offset;
	uint64_t size;
};

struct intel_allocator_simple {
	struct igt_map *objects;
	struct igt_map *reserved;
	struct simple_vma_heap heap;

	uint64_t start;
	uint64_t end;

	/* statistics */
	uint64_t total_size;
	uint64_t allocated_size;
	uint64_t allocated_objects;
	uint64_t reserved_size;
	uint64_t reserved_areas;
};

struct intel_allocator_record {
	uint32_t handle;
	uint64_t offset;
	uint64_t size;
};

#define simple_vma_foreach_hole(_hole, _heap) \
	igt_list_for_each_entry(_hole, &(_heap)->holes, link)

#define simple_vma_foreach_hole_safe(_hole, _heap, _tmp) \
	igt_list_for_each_entry_safe(_hole, _tmp,  &(_heap)->holes, link)

#define simple_vma_foreach_hole_safe_rev(_hole, _heap, _tmp) \
	igt_list_for_each_entry_safe_reverse(_hole, _tmp,  &(_heap)->holes, link)

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

static inline uint32_t hash_offsets(const void *val)
{
	uint64_t hash = *(uint64_t *) val;

	hash = hash * GOLDEN_RATIO_PRIME_64;
	/* High bits are more random, so use them. */
	return hash >> 32;
}

static int equal_offsets(const void *a, const void *b)
{
	uint64_t *key1 = (uint64_t *) a, *key2 = (uint64_t *) b;

	return *key1 == *key2;
}

static void map_entry_free_func(struct igt_map_entry *entry)
{
	free(entry->data);
}

#define GEN8_GTT_ADDRESS_WIDTH 48
#define DECANONICAL(offset) (offset & ((1ull << GEN8_GTT_ADDRESS_WIDTH) - 1))

static void simple_vma_heap_validate(struct simple_vma_heap *heap)
{
	uint64_t prev_offset = 0;
	struct simple_vma_hole *hole;

	simple_vma_foreach_hole(hole, heap) {
		igt_assert(hole->size > 0);

		if (&hole->link == heap->holes.next) {
			/*
			 * This must be the top-most hole.  Assert that,
			 * if it overflows, it overflows to 0, i.e. 2^64.
			 */
			igt_assert(hole->size + hole->offset == 0 ||
				   hole->size + hole->offset > hole->offset);
		} else {
			/*
			 * This is not the top-most hole so it must not overflow and,
			 * in fact, must be strictly lower than the top-most hole.  If
			 * hole->size + hole->offset == prev_offset, then we failed to
			 * join holes during a simple_vma_heap_free.
			 */
			igt_assert(hole->size + hole->offset > hole->offset &&
				   hole->size + hole->offset < prev_offset);
		}
		prev_offset = hole->offset;
	}
}


static void simple_vma_heap_free(struct simple_vma_heap *heap,
				 uint64_t offset, uint64_t size)
{
	struct simple_vma_hole *high_hole = NULL, *low_hole = NULL, *hole;
	bool high_adjacent, low_adjacent;

	/* Freeing something with a size of 0 is not valid. */
	igt_assert(size > 0);

	/*
	 * It's possible for offset + size to wrap around if we touch the top of
	 * the 64-bit address space, but we cannot go any higher than 2^64.
	 */
	igt_assert(offset + size == 0 || offset + size > offset);

	simple_vma_heap_validate(heap);

	/* Find immediately higher and lower holes if they exist. */
	simple_vma_foreach_hole(hole, heap) {
		if (hole->offset <= offset) {
			low_hole = hole;
			break;
		}
		high_hole = hole;
	}

	if (high_hole)
		igt_assert(offset + size <= high_hole->offset);
	high_adjacent = high_hole && offset + size == high_hole->offset;

	if (low_hole) {
		igt_assert(low_hole->offset + low_hole->size > low_hole->offset);
		igt_assert(low_hole->offset + low_hole->size <= offset);
	}
	low_adjacent = low_hole && low_hole->offset + low_hole->size == offset;

	if (low_adjacent && high_adjacent) {
		/* Merge the two holes */
		low_hole->size += size + high_hole->size;
		igt_list_del(&high_hole->link);
		free(high_hole);
	} else if (low_adjacent) {
		/* Merge into the low hole */
		low_hole->size += size;
	} else if (high_adjacent) {
		/* Merge into the high hole */
		high_hole->offset = offset;
		high_hole->size += size;
	} else {
		/* Neither hole is adjacent; make a new one */
		hole = calloc(1, sizeof(*hole));
		igt_assert(hole);

		hole->offset = offset;
		hole->size = size;
		/*
		 * Add it after the high hole so we maintain high-to-low
		 * ordering
		 */
		if (high_hole)
			igt_list_add(&hole->link, &high_hole->link);
		else
			igt_list_add(&hole->link, &heap->holes);
	}

	simple_vma_heap_validate(heap);
}

static void simple_vma_heap_init(struct simple_vma_heap *heap,
				 uint64_t start, uint64_t size,
				 enum allocator_strategy strategy)
{
	IGT_INIT_LIST_HEAD(&heap->holes);
	simple_vma_heap_free(heap, start, size);

	/* Use LOW_TO_HIGH or HIGH_TO_LOW strategy only */
	if (strategy == ALLOC_STRATEGY_LOW_TO_HIGH)
		heap->strategy = strategy;
	else
		heap->strategy = ALLOC_STRATEGY_HIGH_TO_LOW;
}

static void simple_vma_heap_finish(struct simple_vma_heap *heap)
{
	struct simple_vma_hole *hole, *tmp;

	simple_vma_foreach_hole_safe(hole, heap, tmp)
		free(hole);
}

static void simple_vma_hole_alloc(struct simple_vma_hole *hole,
				  uint64_t offset, uint64_t size)
{
	struct simple_vma_hole *high_hole;
	uint64_t waste;

	igt_assert(hole->offset <= offset);
	igt_assert(hole->size >= offset - hole->offset + size);

	if (offset == hole->offset && size == hole->size) {
		/* Just get rid of the hole. */
		igt_list_del(&hole->link);
		free(hole);
		return;
	}

	igt_assert(offset - hole->offset <= hole->size - size);
	waste = (hole->size - size) - (offset - hole->offset);
	if (waste == 0) {
		/* We allocated at the top->  Shrink the hole down. */
		hole->size -= size;
		return;
	}

	if (offset == hole->offset) {
		/* We allocated at the bottom. Shrink the hole up-> */
		hole->offset += size;
		hole->size -= size;
		return;
	}

	/*
	 * We allocated in the middle.  We need to split the old hole into two
	 * holes, one high and one low.
	 */
	high_hole = calloc(1, sizeof(*hole));
	igt_assert(high_hole);

	high_hole->offset = offset + size;
	high_hole->size = waste;

	/*
	 * Adjust the hole to be the amount of space left at he bottom of the
	 * original hole.
	 */
	hole->size = offset - hole->offset;

	/*
	 * Place the new hole before the old hole so that the list is in order
	 * from high to low.
	 */
	igt_list_add_tail(&high_hole->link, &hole->link);
}

static bool simple_vma_heap_alloc(struct simple_vma_heap *heap,
				  uint64_t *offset, uint64_t size,
				  uint64_t alignment,
				  enum allocator_strategy strategy)
{
	struct simple_vma_hole *hole, *tmp;
	uint64_t misalign;

	/* The caller is expected to reject zero-size allocations */
	igt_assert(size > 0);
	igt_assert(alignment > 0);

	simple_vma_heap_validate(heap);

	/* Ensure we support only NONE/LOW_TO_HIGH/HIGH_TO_LOW strategies */
	igt_assert(strategy == ALLOC_STRATEGY_NONE ||
		   strategy == ALLOC_STRATEGY_LOW_TO_HIGH ||
		   strategy == ALLOC_STRATEGY_HIGH_TO_LOW);

	/* Use default strategy chosen on open */
	if (strategy == ALLOC_STRATEGY_NONE)
		strategy = heap->strategy;

	if (strategy == ALLOC_STRATEGY_HIGH_TO_LOW) {
		simple_vma_foreach_hole_safe(hole, heap, tmp) {
			if (size > hole->size)
				continue;
			/*
			 * Compute the offset as the highest address where a chunk of the
			 * given size can be without going over the top of the hole.
			 *
			 * This calculation is known to not overflow because we know that
			 * hole->size + hole->offset can only overflow to 0 and size > 0.
			 */
			*offset = (hole->size - size) + hole->offset;

			/*
			 * Align the offset.  We align down and not up because we are
			 *
			 * allocating from the top of the hole and not the bottom.
			 */
			*offset = (*offset / alignment) * alignment;

			if (*offset < hole->offset)
				continue;

			simple_vma_hole_alloc(hole, *offset, size);
			simple_vma_heap_validate(heap);
			return true;
		}
	} else {
		simple_vma_foreach_hole_safe_rev(hole, heap, tmp) {
			if (size > hole->size)
				continue;

			*offset = hole->offset;

			/* Align the offset */
			misalign = *offset % alignment;
			if (misalign) {
				uint64_t pad = alignment - misalign;

				if (pad > hole->size - size)
					continue;

				*offset += pad;
			}

			simple_vma_hole_alloc(hole, *offset, size);
			simple_vma_heap_validate(heap);
			return true;
		}
	}

	/* Failed to allocate */
	return false;
}

static void intel_allocator_simple_get_address_range(struct intel_allocator *ial,
						     uint64_t *startp,
						     uint64_t *endp)
{
	struct intel_allocator_simple *ials = ial->priv;

	if (startp)
		*startp = ials->start;

	if (endp)
		*endp = ials->end;
}

static bool simple_vma_heap_alloc_addr(struct intel_allocator_simple *ials,
				       uint64_t offset, uint64_t size)
{
	struct simple_vma_heap *heap = &ials->heap;
	struct simple_vma_hole *hole, *tmp;

	/* Allocating something with a size of 0 is not valid. */
	igt_assert(size > 0);

	/*
	 * It's possible for offset + size to wrap around if we touch the top of
	 * the 64-bit address space, but we cannot go any higher than 2^64.
	 */
	igt_assert(offset + size == 0 || offset + size > offset);

	/* Find the hole if one exists. */
	simple_vma_foreach_hole_safe(hole, heap, tmp) {
		if (hole->offset > offset)
			continue;

		/*
		 * Holes are ordered high-to-low so the first hole we find with
		 * hole->offset <= is our hole.  If it's not big enough to contain the
		 * requested range, then the allocation fails.
		 */
		igt_assert(hole->offset <= offset);
		if (hole->size < offset - hole->offset + size)
			return false;

		simple_vma_hole_alloc(hole, offset, size);
		return true;
	}

	/* We didn't find a suitable hole */
	return false;
}

static uint64_t intel_allocator_simple_alloc(struct intel_allocator *ial,
					     uint32_t handle, uint64_t size,
					     uint64_t alignment,
					     enum allocator_strategy strategy)
{
	struct intel_allocator_record *rec;
	struct intel_allocator_simple *ials;
	uint64_t offset;

	igt_assert(ial);
	ials = (struct intel_allocator_simple *) ial->priv;
	igt_assert(ials);
	igt_assert(handle);

	rec = igt_map_search(ials->objects, &handle);
	if (rec) {
		offset = rec->offset;
		igt_assert(rec->size == size);
	} else {
		if (!simple_vma_heap_alloc(&ials->heap, &offset,
					   size, alignment, strategy))
			return ALLOC_INVALID_ADDRESS;

		rec = malloc(sizeof(*rec));
		rec->handle = handle;
		rec->offset = offset;
		rec->size = size;

		igt_map_insert(ials->objects, &rec->handle, rec);
		ials->allocated_objects++;
		ials->allocated_size += size;
	}

	return offset;
}

static bool intel_allocator_simple_free(struct intel_allocator *ial, uint32_t handle)
{
	struct intel_allocator_record *rec = NULL;
	struct intel_allocator_simple *ials;
	struct igt_map_entry *entry;

	igt_assert(ial);
	ials = (struct intel_allocator_simple *) ial->priv;
	igt_assert(ials);

	entry = igt_map_search_entry(ials->objects, &handle);
	if (entry) {
		igt_map_remove_entry(ials->objects, entry);
		if (entry->data) {
			rec = (struct intel_allocator_record *) entry->data;
			simple_vma_heap_free(&ials->heap, rec->offset, rec->size);
			ials->allocated_objects--;
			ials->allocated_size -= rec->size;
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

static bool intel_allocator_simple_is_allocated(struct intel_allocator *ial,
						uint32_t handle, uint64_t size,
						uint64_t offset)
{
	struct intel_allocator_record *rec;
	struct intel_allocator_simple *ials;
	bool same = false;

	igt_assert(ial);
	ials = (struct intel_allocator_simple *) ial->priv;
	igt_assert(ials);
	igt_assert(handle);

	rec = igt_map_search(ials->objects, &handle);
	if (rec && __same(rec, handle, size, offset))
		same = true;

	return same;
}

static uint64_t get_size(uint64_t start, uint64_t end)
{
	end = end ? end : 1ull << GEN8_GTT_ADDRESS_WIDTH;

	return end - start;
}

static bool intel_allocator_simple_reserve(struct intel_allocator *ial,
					   uint32_t handle,
					   uint64_t start, uint64_t end)
{
	uint64_t size;
	struct intel_allocator_record *rec = NULL;
	struct intel_allocator_simple *ials;

	igt_assert(ial);
	ials = (struct intel_allocator_simple *) ial->priv;
	igt_assert(ials);

	/* don't allow end equal to 0 before decanonical */
	igt_assert(end);

	/* clear [63:48] bits to get rid of canonical form */
	start = DECANONICAL(start);
	end = DECANONICAL(end);
	igt_assert(end > start || end == 0);
	size = get_size(start, end);

	if (simple_vma_heap_alloc_addr(ials, start, size)) {
		rec = malloc(sizeof(*rec));
		rec->handle = handle;
		rec->offset = start;
		rec->size = size;

		igt_map_insert(ials->reserved, &rec->offset, rec);

		ials->reserved_areas++;
		ials->reserved_size += rec->size;
		return true;
	}

	igt_debug("Failed to reserve %llx + %llx\n", (long long)start, (long long)size);
	return false;
}

static bool intel_allocator_simple_unreserve(struct intel_allocator *ial,
					     uint32_t handle,
					     uint64_t start, uint64_t end)
{
	uint64_t size;
	struct intel_allocator_record *rec = NULL;
	struct intel_allocator_simple *ials;
	struct igt_map_entry *entry;

	igt_assert(ial);
	ials = (struct intel_allocator_simple *) ial->priv;
	igt_assert(ials);

	/* don't allow end equal to 0 before decanonical */
	igt_assert(end);

	/* clear [63:48] bits to get rid of canonical form */
	start = DECANONICAL(start);
	end = DECANONICAL(end);
	igt_assert(end > start || end == 0);
	size = get_size(start, end);

	entry = igt_map_search_entry(ials->reserved, &start);

	if (!entry || !entry->data) {
		igt_debug("Only reserved blocks can be unreserved\n");
		return false;
	}
	rec = entry->data;

	if (rec->size != size) {
		igt_debug("Only the whole block unreservation allowed\n");
		return false;
	}

	if (rec->handle != handle) {
		igt_debug("Handle %u doesn't match reservation handle: %u\n",
			 rec->handle, handle);
		return false;
	}

	igt_map_remove_entry(ials->reserved, entry);
	ials->reserved_areas--;
	ials->reserved_size -= rec->size;
	free(rec);
	simple_vma_heap_free(&ials->heap, start, size);

	return true;
}

static bool intel_allocator_simple_is_reserved(struct intel_allocator *ial,
					       uint64_t start, uint64_t end)
{
	uint64_t size;
	struct intel_allocator_record *rec = NULL;
	struct intel_allocator_simple *ials;

	igt_assert(ial);
	ials = (struct intel_allocator_simple *) ial->priv;
	igt_assert(ials);

	/* don't allow end equal to 0 before decanonical */
	igt_assert(end);

	/* clear [63:48] bits to get rid of canonical form */
	start = DECANONICAL(start);
	end = DECANONICAL(end);
	igt_assert(end > start || end == 0);
	size = get_size(start, end);

	rec = igt_map_search(ials->reserved, &start);

	if (!rec)
		return false;

	if (rec->offset == start && rec->size == size)
		return true;

	return false;
}

static void intel_allocator_simple_destroy(struct intel_allocator *ial)
{
	struct intel_allocator_simple *ials;

	igt_assert(ial);
	ials = (struct intel_allocator_simple *) ial->priv;
	simple_vma_heap_finish(&ials->heap);

	igt_map_destroy(ials->objects, map_entry_free_func);

	igt_map_destroy(ials->reserved, map_entry_free_func);

	free(ial->priv);
	free(ial);
}

static bool intel_allocator_simple_is_empty(struct intel_allocator *ial)
{
	struct intel_allocator_simple *ials = ial->priv;

	igt_debug("<ial: %p, fd: %d> objects: %" PRId64
		  ", reserved_areas: %" PRId64 "\n",
		  ial, ial->fd,
		  ials->allocated_objects, ials->reserved_areas);

	return !ials->allocated_objects && !ials->reserved_areas;
}

static void intel_allocator_simple_print(struct intel_allocator *ial, bool full)
{
	struct intel_allocator_simple *ials;
	struct simple_vma_hole *hole;
	struct simple_vma_heap *heap;
	struct igt_map_entry *pos;
	uint64_t total_free = 0, allocated_size = 0, allocated_objects = 0;
	uint64_t reserved_size = 0, reserved_areas = 0;

	igt_assert(ial);
	ials = (struct intel_allocator_simple *) ial->priv;
	igt_assert(ials);
	heap = &ials->heap;

	igt_info("intel_allocator_simple <ial: %p, fd: %d> on "
		 "[0x%"PRIx64" : 0x%"PRIx64"]:\n", ial, ial->fd,
		 ials->start, ials->end);

	if (full) {
		igt_info("holes:\n");
		simple_vma_foreach_hole(hole, heap) {
			igt_info("offset = %"PRIu64" (0x%"PRIx64", "
				 "size = %"PRIu64" (0x%"PRIx64")\n",
				 hole->offset, hole->offset, hole->size,
				 hole->size);
			total_free += hole->size;
		}
		igt_assert(total_free <= ials->total_size);
		igt_info("total_free: %" PRIx64
			 ", total_size: %" PRIx64
			 ", allocated_size: %" PRIx64
			 ", reserved_size: %" PRIx64 "\n",
			 total_free, ials->total_size, ials->allocated_size,
			 ials->reserved_size);
		igt_assert(total_free ==
			   ials->total_size - ials->allocated_size - ials->reserved_size);

		igt_info("objects:\n");
		igt_map_foreach(ials->objects, pos) {
			struct intel_allocator_record *rec = pos->data;

			igt_info("handle = %d, offset = %"PRIu64" "
				"(0x%"PRIx64", size = %"PRIu64" (0x%"PRIx64")\n",
				 rec->handle, rec->offset, rec->offset,
				 rec->size, rec->size);
			allocated_objects++;
			allocated_size += rec->size;
		}
		igt_assert(ials->allocated_size == allocated_size);
		igt_assert(ials->allocated_objects == allocated_objects);

		igt_info("reserved areas:\n");
		igt_map_foreach(ials->reserved, pos) {
			struct intel_allocator_record *rec = pos->data;

			igt_info("offset = %"PRIu64" (0x%"PRIx64", "
				 "size = %"PRIu64" (0x%"PRIx64")\n",
				 rec->offset, rec->offset,
				 rec->size, rec->size);
			reserved_areas++;
			reserved_size += rec->size;
		}
		igt_assert(ials->reserved_areas == reserved_areas);
		igt_assert(ials->reserved_size == reserved_size);
	} else {
		simple_vma_foreach_hole(hole, heap)
			total_free += hole->size;
	}

	igt_info("free space: %"PRIu64"B (0x%"PRIx64") (%.2f%% full)\n"
		 "allocated objects: %"PRIu64", reserved areas: %"PRIu64"\n",
		 total_free, total_free,
		 ((double) (ials->total_size - total_free) /
		  (double) ials->total_size) * 100,
		 ials->allocated_objects, ials->reserved_areas);
}

struct intel_allocator *
intel_allocator_simple_create(int fd, uint64_t start, uint64_t end,
			      enum allocator_strategy strategy)
{
	struct intel_allocator *ial;
	struct intel_allocator_simple *ials;

	igt_debug("Using simple allocator\n");

	ial = calloc(1, sizeof(*ial));
	igt_assert(ial);

	ial->fd = fd;
	ial->get_address_range = intel_allocator_simple_get_address_range;
	ial->alloc = intel_allocator_simple_alloc;
	ial->free = intel_allocator_simple_free;
	ial->is_allocated = intel_allocator_simple_is_allocated;
	ial->reserve = intel_allocator_simple_reserve;
	ial->unreserve = intel_allocator_simple_unreserve;
	ial->is_reserved = intel_allocator_simple_is_reserved;
	ial->destroy = intel_allocator_simple_destroy;
	ial->is_empty = intel_allocator_simple_is_empty;
	ial->print = intel_allocator_simple_print;
	ials = ial->priv = malloc(sizeof(struct intel_allocator_simple));
	igt_assert(ials);

	ials->objects = igt_map_create(hash_handles, equal_handles);
	ials->reserved = igt_map_create(hash_offsets, equal_offsets);
	igt_assert(ials->objects && ials->reserved);

	ials->start = start;
	ials->end = end;
	ials->total_size = end - start;
	simple_vma_heap_init(&ials->heap, ials->start, ials->total_size,
			     strategy);

	ials->allocated_size = 0;
	ials->allocated_objects = 0;
	ials->reserved_size = 0;
	ials->reserved_areas = 0;

	return ial;
}
