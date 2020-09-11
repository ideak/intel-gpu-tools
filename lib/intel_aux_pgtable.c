#include <stdbool.h>
#include <stdint.h>

#include "drmtest.h"
#include "intel_aux_pgtable.h"
#include "intel_batchbuffer.h"
#include "intel_bufops.h"
#include "ioctl_wrappers.h"

#include "i915/gem_mman.h"

#define BITS_PER_LONG_LONG	(sizeof(long long) * 8)
#define BITMASK(e, s)		((~0ULL << (s)) & \
				 (~0ULL >> (BITS_PER_LONG_LONG - 1 - (e))))

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

#define AUX_FORMAT_YCRCB	0x03
#define AUX_FORMAT_P010		0x07
#define AUX_FORMAT_P016		0x08
#define AUX_FORMAT_ARGB_8B	0x0A
#define AUX_FORMAT_NV12_21	0x0F

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
	struct intel_bb *ibb;
	struct intel_buf *buf;
	void *ptr;
};

static uint64_t last_buf_surface_end(struct intel_buf *buf)
{
	uint64_t end_offset = 0;
	int num_surfaces = buf->format_is_yuv_semiplanar ? 2 : 1;
	int i;

	for (i = 0; i < num_surfaces; i++) {
		uint64_t surface_end = buf->surface[i].offset +
				       buf->surface[i].size;

		if (surface_end > end_offset)
			end_offset = surface_end;
	}

	return end_offset;
}

static int
pgt_table_count(int address_bits, struct intel_buf **bufs, int buf_count)
{
	uint64_t end;
	int count;
	int i;

	count = 0;
	end = 0;
	for (i = 0; i < buf_count; i++) {
		struct intel_buf *buf = bufs[i];
		uint64_t start;

		/* We require bufs to be sorted. */
		igt_assert(i == 0 ||
			   buf->addr.offset >= bufs[i - 1]->addr.offset +
					       intel_buf_bo_size(bufs[i - 1]));

		start = ALIGN_DOWN(buf->addr.offset, 1UL << address_bits);
		/* Avoid double counting for overlapping aligned bufs. */
		start = max(start, end);

		end = ALIGN(buf->addr.offset + last_buf_surface_end(buf),
			    1UL << address_bits);
		igt_assert(end >= start);

		count += (end - start) >> address_bits;
	}

	return count;
}

static void
pgt_calc_size(struct pgtable *pgt, struct intel_buf **bufs, int buf_count)
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

	parent_table_ptr = pgt->ptr + parent_table;
	child_entry_idx = pgt_entry_index(pgt, level, address);
	child_entry_ptr = &parent_table_ptr[child_entry_idx];

	if (!*child_entry_ptr) {
		uint64_t pte;
		uint32_t offset;

		child_table = pgt_alloc_table(pgt, level - 1);
		igt_assert(!((child_table + pgt->buf->addr.offset) &
			     ~ptr_mask(pgt, level)));

		pte = child_table | flags;
		*child_entry_ptr = pgt->buf->addr.offset + pte;

		igt_assert(pte <= INT32_MAX);

		offset = parent_table + child_entry_idx * sizeof(uint64_t);
		intel_bb_offset_reloc_to_object(pgt->ibb,
						pgt->buf->handle,
						pgt->buf->handle,
						0, 0,
						pte, offset,
						pgt->buf->addr.offset);
	} else {
		child_table = (*child_entry_ptr & ptr_mask(pgt, level)) -
			      pgt->buf->addr.offset;
	}

	return child_table;
}

static void
pgt_set_l1_entry(struct pgtable *pgt, uint64_t l1_table,
		 uint64_t address, uint64_t ptr, uint64_t flags)
{
	uint64_t *l1_table_ptr;
	uint64_t *l1_entry_ptr;

	l1_table_ptr = pgt->ptr + l1_table;
	l1_entry_ptr = &l1_table_ptr[pgt_entry_index(pgt, 0, address)];

	igt_assert(!(ptr & ~ptr_mask(pgt, 0)));
	*l1_entry_ptr = ptr | flags;
}

#define DEPTH_VAL_RESERVED	3

static int bpp_to_depth_val(int bpp)
{
	switch (bpp) {
	case 8:
		return 4;
	case 10:
		return 1;
	case 12:
		return 2;
	case 16:
		return 0;
	case 32:
		return 5;
	case 64:
		return 6;
	default:
		igt_assert_f(0, "invalid bpp %d\n", bpp);
	}
}

static uint64_t pgt_get_l1_flags(const struct intel_buf *buf, int surface_idx)
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

	entry.e.ycr = surface_idx > 0;

	if (buf->format_is_yuv_semiplanar) {
		entry.e.depth = bpp_to_depth_val(buf->bpp);
		switch (buf->yuv_semiplanar_bpp) {
		case 8:
			entry.e.format = AUX_FORMAT_NV12_21;
			entry.e.depth = DEPTH_VAL_RESERVED;
			break;
		case 10:
			entry.e.format = AUX_FORMAT_P010;
			entry.e.depth = bpp_to_depth_val(10);
			break;
		case 12:
			entry.e.format = AUX_FORMAT_P016;
			entry.e.depth = bpp_to_depth_val(12);
			break;
		case 16:
			entry.e.format = AUX_FORMAT_P016;
			entry.e.depth = bpp_to_depth_val(16);
			break;
		default:
			igt_assert(0);
		}
	} else if (buf->format_is_yuv) {
		switch (buf->bpp) {
		case 16:
			entry.e.format = AUX_FORMAT_YCRCB;
			entry.e.depth = DEPTH_VAL_RESERVED;
			break;
		default:
			igt_assert(0);
		}
	} else {
		switch (buf->bpp) {
		case 32:
			entry.e.format = AUX_FORMAT_ARGB_8B;
			entry.e.depth = bpp_to_depth_val(32);
			break;
		default:
			igt_assert(0);
		}
	}

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
			     struct intel_buf *buf,
			     uint64_t top_table,
			     int surface_idx)
{
	uint64_t surface_addr = buf->addr.offset +
				buf->surface[surface_idx].offset;
	uint64_t surface_end = surface_addr +
			       buf->surface[surface_idx].size;
	uint64_t aux_addr = buf->addr.offset + buf->ccs[surface_idx].offset;
	uint64_t l1_flags = pgt_get_l1_flags(buf, surface_idx);
	uint64_t lx_flags = pgt_get_lx_flags();

	igt_assert(!(buf->surface[surface_idx].stride % 512));
	igt_assert_eq(buf->ccs[surface_idx].stride,
		      buf->surface[surface_idx].stride / 512 * 64);

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

static void pgt_map(int i915, struct pgtable *pgt)
{
	pgt->ptr = gem_mmap__device_coherent(i915, pgt->buf->handle, 0,
					     pgt->size, PROT_READ | PROT_WRITE);
}

static void pgt_unmap(struct pgtable *pgt)
{
	munmap(pgt->ptr, pgt->size);
}

static void pgt_populate_entries(struct pgtable *pgt,
				 struct intel_buf **bufs,
				 int buf_count)
{
	uint64_t top_table;
	int i;

	top_table = pgt_alloc_table(pgt, pgt->levels - 1);
	/* Top level table must be at offset 0. */
	igt_assert(top_table == 0);

	for (i = 0; i < buf_count; i++) {
		igt_assert_eq(bufs[i]->surface[0].offset, 0);

		pgt_populate_entries_for_buf(pgt, bufs[i], top_table, 0);
		if (bufs[i]->format_is_yuv_semiplanar)
			pgt_populate_entries_for_buf(pgt, bufs[i], top_table, 1);
	}
}

static struct pgtable *
pgt_create(const struct pgtable_level_desc *level_descs, int levels,
	   struct intel_buf **bufs, int buf_count)
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

struct intel_buf *
intel_aux_pgtable_create(struct intel_bb *ibb,
			 struct intel_buf **bufs, int buf_count)
{
	struct drm_i915_gem_exec_object2 *obj;
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
	struct buf_ops *bops;
	struct intel_buf *buf;
	uint64_t prev_alignment;

	igt_assert(buf_count);
	bops = bufs[0]->bops;

	pgt = pgt_create(level_desc, ARRAY_SIZE(level_desc), bufs, buf_count);
	pgt->ibb = ibb;
	pgt->buf = intel_buf_create(bops, pgt->size, 1, 8, 0, I915_TILING_NONE,
				    I915_COMPRESSION_NONE);

	/* We need to use pgt->max_align for aux table */
	prev_alignment = intel_bb_set_default_object_alignment(ibb,
							       pgt->max_align);
	obj = intel_bb_add_intel_buf(ibb, pgt->buf, false);
	intel_bb_set_default_object_alignment(ibb, prev_alignment);
	obj->alignment = pgt->max_align;

	pgt_map(ibb->i915, pgt);
	pgt_populate_entries(pgt, bufs, buf_count);
	pgt_unmap(pgt);

	buf = pgt->buf;
	pgt_destroy(pgt);

	return buf;
}

static void
aux_pgtable_reserve_buf_slot(struct intel_buf **bufs, int buf_count,
			     struct intel_buf *new_buf)
{
	int i;

	for (i = 0; i < buf_count; i++)
		if (bufs[i]->addr.offset > new_buf->addr.offset)
			break;

	memmove(&bufs[i + 1], &bufs[i], sizeof(bufs[0]) * (buf_count - i));

	bufs[i] = new_buf;
}

void
gen12_aux_pgtable_init(struct aux_pgtable_info *info,
		       struct intel_bb *ibb,
		       struct intel_buf *src_buf,
		       struct intel_buf *dst_buf)
{
	struct intel_buf *bufs[2];
	struct intel_buf *reserved_bufs[2];
	int reserved_buf_count;
	int i;

	igt_assert_f(ibb->enforce_relocs == false,
		     "We support aux pgtables for non-forced relocs yet!");

	if (!intel_buf_compressed(src_buf) && !intel_buf_compressed(dst_buf))
		return;

	bufs[0] = src_buf;
	bufs[1] = dst_buf;

	/*
	 * Surface index in pgt table depend on its address so:
	 *   1. if handle was previously executed in batch use that address
	 *   2. add object to batch, this will generate random address
	 *
	 * Randomizing addresses can lead to overlapping, but we don't have
	 * global address space generator in IGT. Currently assumption is
	 * randomizing address is spread over 48-bit address space equally
	 * so risk with overlapping is minimal. Of course it is growing
	 * with number of objects (+its sizes) involved in blit.
	 * To avoid relocation EXEC_OBJECT_PINNED flag is set for compressed
	 * surfaces.
	 */

	intel_bb_add_intel_buf(ibb, src_buf, false);
	if (intel_buf_compressed(src_buf))
		intel_bb_object_set_flag(ibb, src_buf->handle, EXEC_OBJECT_PINNED);

	intel_bb_add_intel_buf(ibb, dst_buf, true);
	if (intel_buf_compressed(dst_buf))
		intel_bb_object_set_flag(ibb, dst_buf->handle, EXEC_OBJECT_PINNED);

	reserved_buf_count = 0;
	/* First reserve space for any bufs that are bound already. */
	for (i = 0; i < ARRAY_SIZE(bufs); i++) {
		igt_assert(bufs[i]->addr.offset != INTEL_BUF_INVALID_ADDRESS);
		aux_pgtable_reserve_buf_slot(reserved_bufs,
					     reserved_buf_count++,
					     bufs[i]);
	}

	/* Create AUX pgtable entries only for bufs with an AUX surface */
	info->buf_count = 0;
	for (i = 0; i < reserved_buf_count; i++) {
		if (!intel_buf_compressed(reserved_bufs[i]))
			continue;

		info->bufs[info->buf_count] = reserved_bufs[i];
		info->buf_pin_offsets[info->buf_count] =
			reserved_bufs[i]->addr.offset;

		info->buf_count++;
	}

	info->pgtable_buf = intel_aux_pgtable_create(ibb,
						     info->bufs,
						     info->buf_count);

	igt_assert(info->pgtable_buf);
}

void
gen12_aux_pgtable_cleanup(struct intel_bb *ibb, struct aux_pgtable_info *info)
{
	int i;

	/* Check that the pinned bufs kept their offset after the exec. */
	for (i = 0; i < info->buf_count; i++) {
		uint64_t addr;

		addr = intel_bb_get_object_offset(ibb, info->bufs[i]->handle);
		igt_assert_eq_u64(addr, info->buf_pin_offsets[i]);
	}

	if (info->pgtable_buf)
		intel_buf_destroy(info->pgtable_buf);
}

uint32_t
gen12_create_aux_pgtable_state(struct intel_bb *ibb,
			       struct intel_buf *aux_pgtable_buf)
{
	uint64_t *pgtable_ptr;
	uint32_t pgtable_ptr_offset;

	if (!aux_pgtable_buf)
		return 0;

	pgtable_ptr = intel_bb_ptr(ibb);
	pgtable_ptr_offset = intel_bb_offset(ibb);

	*pgtable_ptr = intel_bb_offset_reloc(ibb, aux_pgtable_buf->handle,
					     0, 0,
					     pgtable_ptr_offset,
					     aux_pgtable_buf->addr.offset);
	intel_bb_ptr_add(ibb, sizeof(*pgtable_ptr));

	return pgtable_ptr_offset;
}

void
gen12_emit_aux_pgtable_state(struct intel_bb *ibb, uint32_t state, bool render)
{
	uint32_t table_base_reg = render ? GEN12_GFX_AUX_TABLE_BASE_ADDR :
					   GEN12_VEBOX_AUX_TABLE_BASE_ADDR;

	if (!state)
		return;

	intel_bb_out(ibb, MI_LOAD_REGISTER_MEM_GEN8 | MI_MMIO_REMAP_ENABLE_GEN12);
	intel_bb_out(ibb, table_base_reg);
	intel_bb_emit_reloc(ibb, ibb->handle, 0, 0, state, ibb->batch_offset);

	intel_bb_out(ibb, MI_LOAD_REGISTER_MEM_GEN8 | MI_MMIO_REMAP_ENABLE_GEN12);
	intel_bb_out(ibb, table_base_reg + 4);
	intel_bb_emit_reloc(ibb, ibb->handle, 0, 0, state + 4, ibb->batch_offset);
}
