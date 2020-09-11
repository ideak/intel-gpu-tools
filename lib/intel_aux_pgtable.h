#ifndef __INTEL_AUX_PGTABLE_H__
#define __INTEL_AUX_PGTABLE_H__

#include "intel_bufops.h"

struct aux_pgtable_info {
	int buf_count;
	struct intel_buf *bufs[2];
	uint64_t buf_pin_offsets[2];
	struct intel_buf *pgtable_buf;
};

struct intel_buf *
intel_aux_pgtable_create(struct intel_bb *ibb,
			 struct intel_buf **bufs, int buf_count);

void
gen12_aux_pgtable_init(struct aux_pgtable_info *info,
		       struct intel_bb *ibb,
		       struct intel_buf *src_buf,
		       struct intel_buf *dst_buf);

void
gen12_aux_pgtable_cleanup(struct intel_bb *ibb, struct aux_pgtable_info *info);

uint32_t
gen12_create_aux_pgtable_state(struct intel_bb *batch,
			       struct intel_buf *aux_pgtable_buf);
void
gen12_emit_aux_pgtable_state(struct intel_bb *batch, uint32_t state,
			     bool render);

#endif
