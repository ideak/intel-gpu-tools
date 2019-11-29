#ifndef __INTEL_AUX_PGTABLE_H__
#define __INTEL_AUX_PGTABLE_H__

#include "intel_bufmgr.h"

struct igt_buf;
struct intel_batchbuffer;

struct aux_pgtable_info {
	int buf_count;
	const struct igt_buf *bufs[2];
	uint64_t buf_pin_offsets[2];
	drm_intel_bo *pgtable_bo;
};

drm_intel_bo *
intel_aux_pgtable_create(drm_intel_bufmgr *bufmgr,
			 const struct igt_buf **bufs, int buf_count);

void
gen12_aux_pgtable_init(struct aux_pgtable_info *info,
		       drm_intel_bufmgr *bufmgr,
		       const struct igt_buf *src_buf,
		       const struct igt_buf *dst_buf);

void
gen12_aux_pgtable_cleanup(struct aux_pgtable_info *info);

uint32_t
gen12_create_aux_pgtable_state(struct intel_batchbuffer *batch,
			       drm_intel_bo *aux_pgtable_bo);
void
gen12_emit_aux_pgtable_state(struct intel_batchbuffer *batch, uint32_t state);

#endif
