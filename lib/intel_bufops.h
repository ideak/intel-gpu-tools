#ifndef __INTEL_BUFOPS_H__
#define __INTEL_BUFOPS_H__

#include <stdint.h>
#include "igt_aux.h"

struct buf_ops;

struct intel_buf {
	struct buf_ops *bops;
	uint32_t handle;
	uint32_t stride;
	uint32_t tiling;
	uint32_t bpp;
	uint32_t size;
	uint32_t compression;
	struct {
		uint32_t offset;
		uint32_t stride;
	} aux;
};

static inline unsigned int intel_buf_width(const struct intel_buf *buf)
{
	return buf->stride / (buf->bpp / 8);
}

static inline unsigned int intel_buf_height(const struct intel_buf *buf)
{
	return buf->size / buf->stride;
}

static inline unsigned int
intel_buf_aux_width(int gen, const struct intel_buf *buf)
{
	/*
	 * GEN12+: The AUX CCS unit size is 64 bytes mapping 4 main surface
	 * tiles. Thus the width of the CCS unit is 4*32=128 pixels on the
	 * main surface.
	 */
	if (gen >= 12)
		return DIV_ROUND_UP(intel_buf_width(buf), 128) * 64;

	return DIV_ROUND_UP(intel_buf_width(buf), 1024) * 128;
}

static inline unsigned int
intel_buf_aux_height(int gen, const struct intel_buf *buf)
{
	/*
	 * GEN12+: The AUX CCS unit size is 64 bytes mapping 4 main surface
	 * tiles. Thus the height of the CCS unit is 32 pixel rows on the main
	 * surface.
	 */
	if (gen >= 12)
		return DIV_ROUND_UP(intel_buf_height(buf), 32);

	return DIV_ROUND_UP(intel_buf_height(buf), 512) * 32;
}

struct buf_ops *buf_ops_create(int fd);
void buf_ops_destroy(struct buf_ops *bops);
int buf_ops_getfd(struct buf_ops *bops);

bool buf_ops_set_software_tiling(struct buf_ops *bops,
				 uint32_t tiling,
				 bool use_software_tiling);

void intel_buf_to_linear(struct buf_ops *bops, struct intel_buf *buf,
			 uint32_t *linear);

void linear_to_intel_buf(struct buf_ops *bops, struct intel_buf *buf,
			 uint32_t *linear);

bool buf_ops_has_hw_fence(struct buf_ops *bops, uint32_t tiling);
bool buf_ops_has_tiling_support(struct buf_ops *bops, uint32_t tiling);

void intel_buf_init(struct buf_ops *bops, struct intel_buf *buf,
		    int width, int height, int bpp, int alignment,
		    uint32_t tiling, uint32_t compression);
void intel_buf_close(struct buf_ops *bops, struct intel_buf *buf);

void intel_buf_init_using_handle(struct buf_ops *bops,
				 uint32_t handle,
				 struct intel_buf *buf,
				 int width, int height, int bpp, int alignment,
				 uint32_t req_tiling, uint32_t compression);

#endif
