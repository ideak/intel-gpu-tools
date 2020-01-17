#ifndef __RENDERCOPY_BUFMGR_H__
#define __RENDERCOPY_BUFMGR_H__

#include <stdint.h>
#include "intel_bufops.h"
#include "intel_batchbuffer.h"

struct rendercopy_bufmgr;

struct rendercopy_bufmgr *rendercopy_bufmgr_create(int fd,
						   drm_intel_bufmgr *bufmgr);
void rendercopy_bufmgr_destroy(struct rendercopy_bufmgr *bmgr);

bool rendercopy_bufmgr_set_software_tiling(struct rendercopy_bufmgr *bmgr,
					   uint32_t tiling,
					   bool use_software_tiling);

void igt_buf_to_linear(struct rendercopy_bufmgr *bmgr, struct igt_buf *buf,
		       uint32_t *linear);

void linear_to_igt_buf(struct rendercopy_bufmgr *bmgr, struct igt_buf *buf,
		       uint32_t *linear);

void igt_buf_init(struct rendercopy_bufmgr *bmgr, struct igt_buf *buf,
		  int width, int height, int bpp,
		  uint32_t tiling, uint32_t compression);

#endif
