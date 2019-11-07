#ifndef __INTEL_AUX_PGTABLE_H__
#define __INTEL_AUX_PGTABLE_H__

#include "intel_bufmgr.h"

struct igt_buf;

drm_intel_bo *
intel_aux_pgtable_create(drm_intel_bufmgr *bufmgr,
			 const struct igt_buf **bufs, int buf_count);

#endif
