#include <stdbool.h>
#include <stdint.h>

#include "drmtest.h"
#include "intel_aux_pgtable.h"
#include "intel_batchbuffer.h"
#include "intel_bufmgr.h"
#include "ioctl_wrappers.h"

#include "i915/gem_mman.h"

#define BITS_PER_LONG		(sizeof(long) * 8)
#define BITMASK(e, s)		((~0UL << (s)) & \
				 (~0UL >> (BITS_PER_LONG - 1 - (e))))

/* The unit size to which the AUX CCS surface is aligned to. */
#define AUX_CCS_UNIT_SIZE	64
/*
 * The block size on the AUX CCS surface which is mapped by one L1 AUX
 * pagetable entry.
 */
#define AUX_CCS_BLOCK_SIZE	(4 * AUX_CCS_UNIT_SIZE)
/*
 * The block size on the main surface mapped by one AUX CCS block:
 *   256 bytes per CCS block *
 *   8   bits per byte /
 *   2   bits per main surface CL *
 *   64  bytes per main surface CL
 */
#define MAIN_SURFACE_BLOCK_SIZE	(AUX_CCS_BLOCK_SIZE * 8 / 2 * 64)

#define GFX_ADDRESS_BITS	48

#define max(a, b)		((a) > (b) ? (a) : (b))

struct pgtable_level_desc {
	int idx_shift;
	int idx_bits;
	int entry_ptr_shift;
	int table_size;
};

struct pgtable_level_info {
	const struct pgtable_level_desc *desc;
	int table_count;
	int alloc_base;
	int alloc_ptr;
};

struct pgtable {
	int levels;
	struct pgtable_level_info *level_info;
	int size;
	int max_align;
	drm_intel_bo *bo;
};

static int
pgt_table_count(int address_bits, const struct igt_buf **bufs, int buf_count)
{
	uint64_t end;
	int count;
	int i;

	count = 0;
	end = 0;
	for (i = 0; i < buf_count; i++) {
		const struct igt_buf *buf = bufs[i];
		uint64_t start;

		/* We require bufs to be sorted. */
		igt_assert(i == 0 ||
			   buf->bo->offset64 >= bufs[i - 1]->bo->offset64 +
						bufs[i - 1]->bo->size);

		start = ALIGN_DOWN(buf->bo->offset64, 1UL << address_bits);
		/* Avoid double counting for overlapping aligned bufs. */
		start = max(start, end);

		end = ALIGN(buf->bo->offset64 + buf->size, 1UL << address_bits);
		igt_assert(end >= start);

		count += (end - start) >> address_bits;
	}

	return count;
}

static void
pgt_calc_size(struct pgtable *pgt, const struct igt_buf **bufs, int buf_count)
{
	int level;

	pgt->size = 0;

	for (level = pgt->levels - 1; level >= 0; level--) {
		struct pgtable_level_info *li = &pgt->level_info[level];

		li->alloc_base = ALIGN(pgt->size, li->desc->table_size);
		li->alloc_ptr = li->alloc_base;

		li->table_count = pgt_table_count(li->desc->idx_shift +
						  li->desc->idx_bits,
						  bufs, buf_count);

		pgt->size = li->alloc_base +
			    li->table_count * li->desc->table_size;
	}
}

static uint64_t pgt_alloc_table(struct pgtable *pgt, int level)
{
	struct pgtable_level_info *li = &pgt->level_info[level];
	uint64_t table;

	table = li->alloc_ptr;
	li->alloc_ptr += li->desc->table_size;

	igt_assert(li->alloc_ptr <=
		   li->alloc_base + li->table_count * li->desc->table_size);

	return table;
}

static int pgt_entry_index(struct pgtable *pgt, int level, uint64_t address)
{
	const struct pgtable_level_desc *ld = pgt->level_info[level].desc;
	uint64_t mask = BITMASK(ld->idx_shift + ld->idx_bits - 1,
				ld->idx_shift);

	return (address & mask) >> ld->idx_shift;
}

static uint64_t ptr_mask(struct pgtable *pgt, int level)
{
	const struct pgtable_level_desc *ld = pgt->level_info[level].desc;

	return BITMASK(GFX_ADDRESS_BITS - 1, ld->entry_ptr_shift);
}

static uint64_t
pgt_get_child_table(struct pgtable *pgt, uint64_t parent_table,
		    int level, uint64_t address, uint64_t flags)
{
	uint64_t *parent_table_ptr;
	int child_entry_idx;
	uint64_t *child_entry_ptr;
	uint64_t child_table;

	parent_table_ptr = pgt->bo->virtual + parent_table;
	child_entry_idx = pgt_entry_index(pgt, level, address);
	child_entry_ptr = &parent_table_ptr[child_entry_idx];

	if (!*child_entry_ptr) {
		uint64_t pte;

		child_table = pgt_alloc_table(pgt, level - 1);
		igt_assert(!((child_table + pgt->bo->offset64) &
			     ~ptr_mask(pgt, level)));

		pte = child_table | flags;
		*child_entry_ptr = pgt->bo->offset64 + pte;

		igt_assert(pte <= INT32_MAX);
		drm_intel_bo_emit_reloc(pgt->bo,
					parent_table +
						child_entry_idx * sizeof(uint64_t),
					pgt->bo, pte, 0, 0);
	} else {
		child_table = (*child_entry_ptr & ptr_mask(pgt, level)) -
			      pgt->bo->offset64;
	}

	return child_table;
}

static void
pgt_set_l1_entry(struct pgtable *pgt, uint64_t l1_table,
		 uint64_t address, uint64_t ptr, uint64_t flags)
{
	uint64_t *l1_table_ptr;
	uint64_t *l1_entry_ptr;

	l1_table_ptr = pgt->bo->virtual + l1_table;
	l1_entry_ptr = &l1_table_ptr[pgt_entry_index(pgt, 0, address)];

	igt_assert(!(ptr & ~ptr_mask(pgt, 0)));
	*l1_entry_ptr = ptr | flags;
}

static uint64_t pgt_get_l1_flags(const struct igt_buf *buf)
{
	/*
	 * The offset of .tile_mode isn't specifed by bspec, it's what Mesa
	 * uses.
	 */
	union {
		struct {
			uint64_t	valid:1;
			uint64_t	compression_mod:2;
			uint64_t	lossy_compression:1;
			uint64_t	pad:4;
			uint64_t	addr:40;
			uint64_t	pad2:4;
			uint64_t	tile_mode:2;
			uint64_t	depth:3;
			uint64_t	ycr:1;
			uint64_t	format:6;
		} e;
		uint64_t l;
	} entry = {
		.e = {
			.valid = 1,
			.tile_mode = buf->tiling == I915_TILING_Y ? 1 : 0,
			.depth = 5,		/* 32bpp */
			.format = 0xA,		/* B8G8R8A8_UNORM */
		}
	};

	/*
	 * TODO: Clarify if Yf is supported and if we need to differentiate
	 *       Ys and Yf.
	 *       Add support for more formats.
	 */
	igt_assert(buf->tiling == I915_TILING_Y ||
		   buf->tiling == I915_TILING_Yf ||
		   buf->tiling == I915_TILING_Ys);

	igt_assert(buf->bpp == 32);

	return entry.l;
}

static uint64_t pgt_get_lx_flags(void)
{
	union {
		struct {
			uint64_t        valid:1;
			uint64_t        addr:47;
			uint64_t        pad:16;
		} e;
		uint64_t l;
	} entry = {
		.e = {
			.valid = 1,
		}
	};

	return entry.l;
}

static void
pgt_populate_entries_for_buf(struct pgtable *pgt,
			       const struct igt_buf *buf,
			       uint64_t top_table)
{
	uint64_t surface_addr = buf->bo->offset64;
	uint64_t surface_end = surface_addr + buf->size;
	uint64_t aux_addr = buf->bo->offset64 + buf->aux.offset;
	uint64_t l1_flags = pgt_get_l1_flags(buf);
	uint64_t lx_flags = pgt_get_lx_flags();

	for (; surface_addr < surface_end;
	     surface_addr += MAIN_SURFACE_BLOCK_SIZE,
	     aux_addr += AUX_CCS_BLOCK_SIZE) {
		uint64_t table = top_table;
		int level;

		for (level = pgt->levels - 1; level >= 1; level--)
			table = pgt_get_child_table(pgt, table, level,
						    surface_addr, lx_flags);

		pgt_set_l1_entry(pgt, table, surface_addr, aux_addr, l1_flags);
	}
}

static void pgt_populate_entries(struct pgtable *pgt,
				 const struct igt_buf **bufs,
				 int buf_count,
				 drm_intel_bo *pgt_bo)
{
	uint64_t top_table;
	int i;

	pgt->bo = pgt_bo;

	igt_assert(pgt_bo->size >= pgt->size);
	memset(pgt_bo->virtual, 0, pgt->size);

	top_table = pgt_alloc_table(pgt, pgt->levels - 1);
	/* Top level table must be at offset 0. */
	igt_assert(top_table == 0);

	for (i = 0; i < buf_count; i++)
		pgt_populate_entries_for_buf(pgt, bufs[i], top_table);
}

static struct pgtable *
pgt_create(const struct pgtable_level_desc *level_descs, int levels,
	   const struct igt_buf **bufs, int buf_count)
{
	struct pgtable *pgt;
	int level;

	pgt = calloc(1, sizeof(*pgt));
	igt_assert(pgt);

	pgt->levels = levels;

	pgt->level_info = calloc(levels, sizeof(*pgt->level_info));
	igt_assert(pgt->level_info);

	for (level = 0; level < pgt->levels; level++) {
		struct pgtable_level_info *li = &pgt->level_info[level];

		li->desc = &level_descs[level];
		if (li->desc->table_size > pgt->max_align)
			pgt->max_align = li->desc->table_size;
	}

	pgt_calc_size(pgt, bufs, buf_count);

	return pgt;
}

static void pgt_destroy(struct pgtable *pgt)
{
	free(pgt->level_info);
	free(pgt);
}

drm_intel_bo *
intel_aux_pgtable_create(drm_intel_bufmgr *bufmgr,
		       const struct igt_buf **bufs, int buf_count)
{
	static const struct pgtable_level_desc level_desc[] = {
		{
			.idx_shift = 16,
			.idx_bits = 8,
			.entry_ptr_shift = 8,
			.table_size = 8 * 1024,
		},
		{
			.idx_shift = 24,
			.idx_bits = 12,
			.entry_ptr_shift = 13,
			.table_size = 32 * 1024,
		},
		{
			.idx_shift = 36,
			.idx_bits = 12,
			.entry_ptr_shift = 15,
			.table_size = 32 * 1024,
		},
	};
	struct pgtable *pgt;
	drm_intel_bo *pgt_bo;

	pgt = pgt_create(level_desc, ARRAY_SIZE(level_desc), bufs, buf_count);

	pgt_bo = drm_intel_bo_alloc_for_render(bufmgr, "aux pgt",
					       pgt->size, pgt->max_align);
	igt_assert(pgt_bo);

	igt_assert(drm_intel_bo_map(pgt_bo, true) == 0);
	pgt_populate_entries(pgt, bufs, buf_count, pgt_bo);
	igt_assert(drm_intel_bo_unmap(pgt_bo) == 0);

	pgt_destroy(pgt);

	return pgt_bo;
}
