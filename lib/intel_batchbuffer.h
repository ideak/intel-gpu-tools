#ifndef INTEL_BATCHBUFFER_H
#define INTEL_BATCHBUFFER_H

#include <stdint.h>
#include <intel_bufmgr.h>
#include "igt_core.h"
#include "intel_reg.h"

#define BATCH_SZ 4096
#define BATCH_RESERVED 16

struct intel_batchbuffer {
	drm_intel_bufmgr *bufmgr;
	uint32_t devid;
	int gen;

	drm_intel_context *ctx;
	drm_intel_bo *bo;

	uint8_t buffer[BATCH_SZ];
	uint8_t *ptr, *end;
	uint8_t *state;
};

struct intel_batchbuffer *intel_batchbuffer_alloc(drm_intel_bufmgr *bufmgr,
						  uint32_t devid);

void intel_batchbuffer_set_context(struct intel_batchbuffer *batch,
				   drm_intel_context *ctx);


void intel_batchbuffer_free(struct intel_batchbuffer *batch);


void intel_batchbuffer_flush(struct intel_batchbuffer *batch);
void intel_batchbuffer_flush_on_ring(struct intel_batchbuffer *batch, int ring);
void intel_batchbuffer_flush_with_context(struct intel_batchbuffer *batch,
					  drm_intel_context *context);

void intel_batchbuffer_reset(struct intel_batchbuffer *batch);

void intel_batchbuffer_data(struct intel_batchbuffer *batch,
                            const void *data, unsigned int bytes);

void intel_batchbuffer_emit_reloc(struct intel_batchbuffer *batch,
				  drm_intel_bo *buffer,
				  uint64_t delta,
				  uint32_t read_domains,
				  uint32_t write_domain,
				  int fenced);

/* Inline functions - might actually be better off with these
 * non-inlined.  Certainly better off switching all command packets to
 * be passed as structs rather than dwords, but that's a little bit of
 * work...
 */
#pragma GCC diagnostic ignored "-Winline"
static inline unsigned int
intel_batchbuffer_space(struct intel_batchbuffer *batch)
{
	return (BATCH_SZ - BATCH_RESERVED) - (batch->ptr - batch->buffer);
}


static inline void
intel_batchbuffer_emit_dword(struct intel_batchbuffer *batch, uint32_t dword)
{
	igt_assert(intel_batchbuffer_space(batch) >= 4);
	*(uint32_t *) (batch->ptr) = dword;
	batch->ptr += 4;
}

static inline void
intel_batchbuffer_require_space(struct intel_batchbuffer *batch,
                                unsigned int sz)
{
	igt_assert(sz < BATCH_SZ - BATCH_RESERVED);
	if (intel_batchbuffer_space(batch) < sz)
		intel_batchbuffer_flush(batch);
}

/**
 * BEGIN_BATCH:
 * @n: number of DWORDS to emit
 * @r: number of RELOCS to emit
 *
 * Prepares a batch to emit @n DWORDS, flushing it if there's not enough space
 * available.
 *
 * This macro needs a pointer to an #intel_batchbuffer structure called batch in
 * scope.
 */
#define BEGIN_BATCH(n, r) do {						\
	int __n = (n); \
	igt_assert(batch->end == NULL); \
	if (batch->gen >= 8) __n += r;	\
	__n *= 4; \
	intel_batchbuffer_require_space(batch, __n);			\
	batch->end = batch->ptr + __n; \
} while (0)

/**
 * OUT_BATCH:
 * @d: DWORD to emit
 *
 * Emits @d into a batch.
 *
 * This macro needs a pointer to an #intel_batchbuffer structure called batch in
 * scope.
 */
#define OUT_BATCH(d) intel_batchbuffer_emit_dword(batch, d)

/**
 * OUT_RELOC_FENCED:
 * @buf: relocation target libdrm buffer object
 * @read_domains: gem domain bits for the relocation
 * @write_domain: gem domain bit for the relocation
 * @delta: delta value to add to @buffer's gpu address
 *
 * Emits a fenced relocation into a batch.
 *
 * This macro needs a pointer to an #intel_batchbuffer structure called batch in
 * scope.
 */
#define OUT_RELOC_FENCED(buf, read_domains, write_domain, delta) do {		\
	igt_assert((delta) >= 0);						\
	intel_batchbuffer_emit_reloc(batch, buf, delta,			\
				     read_domains, write_domain, 1);	\
} while (0)

/**
 * OUT_RELOC:
 * @buf: relocation target libdrm buffer object
 * @read_domains: gem domain bits for the relocation
 * @write_domain: gem domain bit for the relocation
 * @delta: delta value to add to @buffer's gpu address
 *
 * Emits a normal, unfenced relocation into a batch.
 *
 * This macro needs a pointer to an #intel_batchbuffer structure called batch in
 * scope.
 */
#define OUT_RELOC(buf, read_domains, write_domain, delta) do {		\
	igt_assert((delta) >= 0);						\
	intel_batchbuffer_emit_reloc(batch, buf, delta,			\
				     read_domains, write_domain, 0);	\
} while (0)

/**
 * ADVANCE_BATCH:
 *
 * Completes the batch command emission sequence started with #BEGIN_BATCH.
 *
 * This macro needs a pointer to an #intel_batchbuffer structure called batch in
 * scope.
 */
#define ADVANCE_BATCH() do {						\
	igt_assert(batch->ptr == batch->end); \
	batch->end = NULL; \
} while(0)

#define BLIT_COPY_BATCH_START(flags) do { \
	BEGIN_BATCH(8, 2); \
	OUT_BATCH(XY_SRC_COPY_BLT_CMD | \
		  XY_SRC_COPY_BLT_WRITE_ALPHA | \
		  XY_SRC_COPY_BLT_WRITE_RGB | \
		  (flags) | \
		  (6 + 2*(batch->gen >= 8))); \
} while(0)

#define COLOR_BLIT_COPY_BATCH_START(flags) do { \
	BEGIN_BATCH(6, 1); \
	OUT_BATCH(XY_COLOR_BLT_CMD_NOLEN | \
		  COLOR_BLT_WRITE_ALPHA | \
		  XY_COLOR_BLT_WRITE_RGB | \
		  (4 + (batch->gen >= 8))); \
} while(0)

void
intel_blt_copy(struct intel_batchbuffer *batch,
	      drm_intel_bo *src_bo, int src_x1, int src_y1, int src_pitch,
	      drm_intel_bo *dst_bo, int dst_x1, int dst_y1, int dst_pitch,
	      int width, int height, int bpp);
void intel_copy_bo(struct intel_batchbuffer *batch,
		   drm_intel_bo *dst_bo, drm_intel_bo *src_bo,
		   long int size);

/**
 * igt_buf:
 * @bo: underlying libdrm buffer object
 * @stride: stride of the buffer
 * @tiling: tiling mode bits
 * @data: pointer to the memory mapping of the buffer
 * @size: size of the buffer object
 *
 * This is a i-g-t buffer object wrapper structure which augments the baseline
 * libdrm buffer object with suitable data needed by the render copy and the
 * media fill functions.
 */
struct igt_buf {
    drm_intel_bo *bo;
    uint32_t stride;
    uint32_t tiling;
    uint32_t *data;
    uint32_t size;
    /*< private >*/
    unsigned num_tiles;
};

unsigned igt_buf_width(struct igt_buf *buf);
unsigned igt_buf_height(struct igt_buf *buf);

/**
 * igt_render_copyfunc_t:
 * @batch: batchbuffer object
 * @context: libdrm hardware context to use
 * @src: source i-g-t buffer object
 * @src_x: source pixel x-coordination
 * @src_y: source pixel y-coordination
 * @width: width of the copied rectangle
 * @height: height of the copied rectangle
 * @dst: destination i-g-t buffer object
 * @dst_x: destination pixel x-coordination
 * @dst_y: destination pixel y-coordination
 *
 * This is the type of the per-platform render copy functions. The
 * platform-specific implementation can be obtained by calling
 * igt_get_render_copyfunc().
 *
 * A render copy function will emit a batchbuffer to the kernel which executes
 * the specified blit copy operation using the render engine. @context is
 * optional and can be NULL.
 */
typedef void (*igt_render_copyfunc_t)(struct intel_batchbuffer *batch,
				      drm_intel_context *context,
				      struct igt_buf *src, unsigned src_x, unsigned src_y,
				      unsigned width, unsigned height,
				      struct igt_buf *dst, unsigned dst_x, unsigned dst_y);

igt_render_copyfunc_t igt_get_render_copyfunc(int devid);

/**
 * igt_media_fillfunc_t:
 * @batch: batchbuffer object
 * @dst: destination i-g-t buffer object
 * @x: destination pixel x-coordination
 * @y: destination pixel y-coordination
 * @width: width of the filled rectangle
 * @height: height of the filled rectangle
 * @color: fill color to use
 *
 * This is the type of the per-platform media fill functions. The
 * platform-specific implementation can be obtained by calling
 * igt_get_media_fillfunc().
 *
 * A media fill function will emit a batchbuffer to the kernel which executes
 * the specified blit fill operation using the media engine.
 */
typedef void (*igt_media_fillfunc_t)(struct intel_batchbuffer *batch,
				     struct igt_buf *dst,
				     unsigned x, unsigned y,
				     unsigned width, unsigned height,
				     uint8_t color);

igt_media_fillfunc_t igt_get_media_fillfunc(int devid);

#endif
