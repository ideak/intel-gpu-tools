#ifndef INTEL_BATCHBUFFER_H
#define INTEL_BATCHBUFFER_H

#include <stdint.h>
#include <intel_bufmgr.h>
#include <i915_drm.h>

#include "igt_core.h"
#include "intel_reg.h"
#include "drmtest.h"

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

uint32_t intel_batchbuffer_copy_data(struct intel_batchbuffer *batch,
				const void *data, unsigned int bytes,
				uint32_t align);

void intel_batchbuffer_emit_reloc(struct intel_batchbuffer *batch,
				  drm_intel_bo *buffer,
				  uint64_t delta,
				  uint32_t read_domains,
				  uint32_t write_domain,
				  int fenced);

uint32_t
intel_batchbuffer_align(struct intel_batchbuffer *batch, uint32_t align);

void *
intel_batchbuffer_subdata_alloc(struct intel_batchbuffer *batch,
				uint32_t size, uint32_t align);

uint32_t
intel_batchbuffer_subdata_offset(struct intel_batchbuffer *batch, void *ptr);

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
		  (flags) | \
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

/*
 * Yf/Ys tiling
 *
 * Tiling mode in the I915_TILING_... namespace for new tiling modes which are
 * defined in the kernel. (They are not fenceable so the kernel does not need
 * to know about them.)
 *
 * They are to be used the the blitting routines below.
 */
#define I915_TILING_Yf	3
#define I915_TILING_Ys	4

enum i915_compression {
	I915_COMPRESSION_NONE,
	I915_COMPRESSION_RENDER,
	I915_COMPRESSION_MEDIA,
};

/**
 * igt_buf:
 * @bo: underlying libdrm buffer object
 * @stride: stride of the buffer
 * @tiling: tiling mode bits
 * @compression: memory compression mode
 * @bpp: bits per pixel, 8, 16 or 32.
 * @data: pointer to the memory mapping of the buffer
 * @size: size of the buffer object
 *
 * This is a i-g-t buffer object wrapper structure which augments the baseline
 * libdrm buffer object with suitable data needed by the render/vebox copy and
 * the fill functions.
 */
struct igt_buf {
	drm_intel_bo *bo;
	uint32_t tiling;
	enum i915_compression compression;
	uint32_t bpp;
	uint32_t yuv_semiplanar_bpp;
	uint32_t *data;
	bool format_is_yuv:1;
	bool format_is_yuv_semiplanar:1;
	struct {
		uint32_t offset;
		uint32_t stride;
		uint32_t size;
	} surface[2];
	struct {
		uint32_t offset;
		uint32_t stride;
	} ccs[2];
	struct {
		uint32_t offset;
	} cc;
	/*< private >*/
	unsigned num_tiles;
};

static inline bool igt_buf_compressed(const struct igt_buf *buf)
{
	return buf->compression != I915_COMPRESSION_NONE;
}

unsigned igt_buf_width(const struct igt_buf *buf);
unsigned igt_buf_height(const struct igt_buf *buf);
unsigned int igt_buf_intel_ccs_width(int gen, const struct igt_buf *buf);
unsigned int igt_buf_intel_ccs_height(int gen, const struct igt_buf *buf);

void igt_blitter_src_copy(int fd,
			  /* src */
			  uint32_t src_handle,
			  uint32_t src_delta,
			  uint32_t src_stride,
			  uint32_t src_tiling,
			  uint32_t src_x, uint32_t src_y,

			  /* size */
			  uint32_t width, uint32_t height,

			  /* bpp */
			  uint32_t bpp,

			  /* dst */
			  uint32_t dst_handle,
			  uint32_t dst_delta,
			  uint32_t dst_stride,
			  uint32_t dst_tiling,
			  uint32_t dst_x, uint32_t dst_y);

void igt_blitter_fast_copy(struct intel_batchbuffer *batch,
			   const struct igt_buf *src, unsigned src_delta,
			   unsigned src_x, unsigned src_y,
			   unsigned width, unsigned height,
			   int bpp,
			   const struct igt_buf *dst, unsigned dst_delta,
			   unsigned dst_x, unsigned dst_y);

void igt_blitter_fast_copy__raw(int fd,
				/* src */
				uint32_t src_handle,
				unsigned int src_delta,
				unsigned int src_stride,
				unsigned int src_tiling,
				unsigned int src_x, unsigned src_y,

				/* size */
				unsigned int width, unsigned int height,

				/* bpp */
				int bpp,

				/* dst */
				uint32_t dst_handle,
				unsigned int dst_delta,
				unsigned int dst_stride,
				unsigned int dst_tiling,
				unsigned int dst_x, unsigned dst_y);

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
				      const struct igt_buf *src, unsigned src_x, unsigned src_y,
				      unsigned width, unsigned height,
				      const struct igt_buf *dst, unsigned dst_x, unsigned dst_y);

igt_render_copyfunc_t igt_get_render_copyfunc(int devid);


/**
 * igt_vebox_copyfunc_t:
 * @batch: batchbuffer object
 * @src: source i-g-t buffer object
 * @width: width of the copied rectangle
 * @height: height of the copied rectangle
 * @dst: destination i-g-t buffer object
 *
 * This is the type of the per-platform vebox copy functions. The
 * platform-specific implementation can be obtained by calling
 * igt_get_vebox_copyfunc().
 *
 * A vebox copy function will emit a batchbuffer to the kernel which executes
 * the specified blit copy operation using the vebox engine.
 */
typedef void (*igt_vebox_copyfunc_t)(struct intel_batchbuffer *batch,
				     const struct igt_buf *src,
				     unsigned width, unsigned height,
				     const struct igt_buf *dst);

igt_vebox_copyfunc_t igt_get_vebox_copyfunc(int devid);

/**
 * igt_fillfunc_t:
 * @i915: drm fd
 * @buf: destination intel_buf object
 * @x: destination pixel x-coordination
 * @y: destination pixel y-coordination
 * @width: width of the filled rectangle
 * @height: height of the filled rectangle
 * @color: fill color to use
 *
 * This is the type of the per-platform fill functions using media
 * or gpgpu pipeline. The platform-specific implementation can be obtained
 * by calling igt_get_media_fillfunc() or igt_get_gpgpu_fillfunc().
 *
 * A fill function will emit a batchbuffer to the kernel which executes
 * the specified blit fill operation using the media/gpgpu engine.
 */
struct intel_buf;
typedef void (*igt_fillfunc_t)(int i915,
			       struct intel_buf *buf,
			       unsigned x, unsigned y,
			       unsigned width, unsigned height,
			       uint8_t color);

igt_fillfunc_t igt_get_gpgpu_fillfunc(int devid);
igt_fillfunc_t igt_get_media_fillfunc(int devid);

typedef void (*igt_vme_func_t)(int i915,
			       uint32_t ctx,
			       struct intel_buf *src,
			       unsigned int width, unsigned int height,
			       struct intel_buf *dst);

igt_vme_func_t igt_get_media_vme_func(int devid);

/**
 * igt_media_spinfunc_t:
 * @i915: drm fd
 * @buf: destination buffer object
 * @spins: number of loops to execute
 *
 * This is the type of the per-platform media spin functions. The
 * platform-specific implementation can be obtained by calling
 * igt_get_media_spinfunc().
 *
 * The media spin function emits a batchbuffer for the render engine with
 * the media pipeline selected. The workload consists of a single thread
 * which spins in a tight loop the requested number of times. Each spin
 * increments a counter whose final 32-bit value is written to the
 * destination buffer on completion. This utility provides a simple way
 * to keep the render engine busy for a set time for various tests.
 */
typedef void (*igt_media_spinfunc_t)(int i915,
				     struct intel_buf *buf, uint32_t spins);

igt_media_spinfunc_t igt_get_media_spinfunc(int devid);


/*
 * Batchbuffer without libdrm dependency
 */
struct intel_bb {
	int i915;
	int gen;
	bool debug;
	bool dump_base64;
	bool enforce_relocs;
	uint32_t devid;
	uint32_t handle;
	uint32_t size;
	uint32_t *batch;
	uint32_t *ptr;
	uint64_t alignment;
	int fence;

	uint32_t prng;
	uint64_t gtt_size;
	bool supports_48b_address;

	uint32_t ctx;

	/* Cache */
	void *root;

	/* Current objects for execbuf */
	void *current;

	/* Objects for current execbuf */
	struct drm_i915_gem_exec_object2 **objects;
	uint32_t num_objects;
	uint32_t allocated_objects;
	uint64_t batch_offset;

	struct drm_i915_gem_relocation_entry *relocs;
	uint32_t num_relocs;
	uint32_t allocated_relocs;

	/*
	 * BO recreate in reset path only when refcount == 0
	 * Currently we don't need to use atomics because intel_bb
	 * is not thread-safe.
	 */
	int32_t refcount;
};

struct intel_bb *intel_bb_create(int i915, uint32_t size);
struct intel_bb *intel_bb_create_with_relocs(int i915, uint32_t size);
void intel_bb_destroy(struct intel_bb *ibb);

static inline void intel_bb_ref(struct intel_bb *ibb)
{
	ibb->refcount++;
}

static inline void intel_bb_unref(struct intel_bb *ibb)
{
	igt_assert_f(ibb->refcount > 0, "intel_bb refcount is 0!");
	ibb->refcount--;
}

void intel_bb_reset(struct intel_bb *ibb, bool purge_objects_cache);
int intel_bb_sync(struct intel_bb *ibb);
void intel_bb_print(struct intel_bb *ibb);
void intel_bb_dump(struct intel_bb *ibb, const char *filename);
void intel_bb_set_debug(struct intel_bb *ibb, bool debug);
void intel_bb_set_dump_base64(struct intel_bb *ibb, bool dump);

static inline uint64_t
intel_bb_set_default_object_alignment(struct intel_bb *ibb, uint64_t alignment)
{
	uint64_t old = ibb->alignment;

	ibb->alignment = alignment;

	return old;
}

static inline uint64_t
intel_bb_get_default_object_alignment(struct intel_bb *ibb)
{
	return ibb->alignment;
}

static inline uint32_t intel_bb_offset(struct intel_bb *ibb)
{
	return (uint32_t) ((uint8_t *) ibb->ptr - (uint8_t *) ibb->batch);
}

static inline void intel_bb_ptr_set(struct intel_bb *ibb, uint32_t offset)
{
	ibb->ptr = (void *) ((uint8_t *) ibb->batch + offset);

	igt_assert(intel_bb_offset(ibb) <= ibb->size);
}

static inline void intel_bb_ptr_add(struct intel_bb *ibb, uint32_t offset)
{
	intel_bb_ptr_set(ibb, intel_bb_offset(ibb) + offset);
}

static inline uint32_t intel_bb_ptr_add_return_prev_offset(struct intel_bb *ibb,
							   uint32_t offset)
{
	uint32_t previous_offset = intel_bb_offset(ibb);

	intel_bb_ptr_set(ibb, previous_offset + offset);

	return previous_offset;
}

static inline void *intel_bb_ptr_align(struct intel_bb *ibb, uint32_t alignment)
{
	intel_bb_ptr_set(ibb, ALIGN(intel_bb_offset(ibb), alignment));

	return (void *) ibb->ptr;
}

static inline void *intel_bb_ptr(struct intel_bb *ibb)
{
	return (void *) ibb->ptr;
}

static inline void intel_bb_out(struct intel_bb *ibb, uint32_t dword)
{
	*ibb->ptr = dword;
	ibb->ptr++;

	igt_assert(intel_bb_offset(ibb) <= ibb->size);
}


struct drm_i915_gem_exec_object2 *
intel_bb_add_object(struct intel_bb *ibb, uint32_t handle,
		    uint64_t offset, bool write);
struct drm_i915_gem_exec_object2 *
intel_bb_add_intel_buf(struct intel_bb *ibb, struct intel_buf *buf, bool write);

struct drm_i915_gem_exec_object2 *
intel_bb_find_object(struct intel_bb *ibb, uint32_t handle);

bool
intel_bb_object_set_flag(struct intel_bb *ibb, uint32_t handle, uint64_t flag);
bool
intel_bb_object_clear_flag(struct intel_bb *ibb, uint32_t handle, uint64_t flag);

uint64_t intel_bb_emit_reloc(struct intel_bb *ibb,
			 uint32_t handle,
			 uint32_t read_domains,
			 uint32_t write_domain,
			 uint64_t delta,
			 uint64_t presumed_offset);

uint64_t intel_bb_emit_reloc_fenced(struct intel_bb *ibb,
				    uint32_t handle,
				    uint32_t read_domains,
				    uint32_t write_domain,
				    uint64_t delta,
				    uint64_t presumed_offset);

uint64_t intel_bb_offset_reloc(struct intel_bb *ibb,
			       uint32_t handle,
			       uint32_t read_domains,
			       uint32_t write_domain,
			       uint32_t offset,
			       uint64_t presumed_offset);

uint64_t intel_bb_offset_reloc_with_delta(struct intel_bb *ibb,
					  uint32_t handle,
					  uint32_t read_domains,
					  uint32_t write_domain,
					  uint32_t delta,
					  uint32_t offset,
					  uint64_t presumed_offset);

uint64_t intel_bb_offset_reloc_to_object(struct intel_bb *ibb,
					 uint32_t handle,
					 uint32_t to_handle,
					 uint32_t read_domains,
					 uint32_t write_domain,
					 uint32_t delta,
					 uint32_t offset,
					 uint64_t presumed_offset);

int __intel_bb_exec(struct intel_bb *ibb, uint32_t end_offset,
		    uint32_t ctx, uint64_t flags, bool sync);

void intel_bb_exec(struct intel_bb *ibb, uint32_t end_offset,
		   uint64_t flags, bool sync);

void intel_bb_exec_with_context(struct intel_bb *ibb, uint32_t end_offset,
				uint32_t ctx, uint64_t flags, bool sync);

uint64_t intel_bb_get_object_offset(struct intel_bb *ibb, uint32_t handle);
bool intel_bb_object_offset_to_buf(struct intel_bb *ibb, struct intel_buf *buf);

uint32_t intel_bb_emit_bbe(struct intel_bb *ibb);
uint32_t intel_bb_emit_flush_common(struct intel_bb *ibb);
void intel_bb_flush(struct intel_bb *ibb, uint32_t ctx, uint32_t ring);
void intel_bb_flush_render(struct intel_bb *ibb);
void intel_bb_flush_render_with_context(struct intel_bb *ibb, uint32_t ctx);
void intel_bb_flush_blit(struct intel_bb *ibb);
void intel_bb_flush_blit_with_context(struct intel_bb *ibb, uint32_t ctx);

uint32_t intel_bb_copy_data(struct intel_bb *ibb,
			    const void *data, unsigned int bytes,
			    uint32_t align);

void intel_bb_blit_start(struct intel_bb *ibb, uint32_t flags);
void intel_bb_emit_blt_copy(struct intel_bb *ibb,
			    struct intel_buf *src,
			    int src_x1, int src_y1, int src_pitch,
			    struct intel_buf *dst,
			    int dst_x1, int dst_y1, int dst_pitch,
			    int width, int height, int bpp);
void intel_bb_blt_copy(struct intel_bb *ibb,
		       struct intel_buf *src,
		       int src_x1, int src_y1, int src_pitch,
		       struct intel_buf *dst,
		       int dst_x1, int dst_y1, int dst_pitch,
		       int width, int height, int bpp);

/**
 * igt_huc_copyfunc_t:
 * @fd: drm fd
 * @obj: drm_i915_gem_exec_object2 buffer array
 *       obj[0] is source buffer
 *       obj[1] is destination buffer
 *       obj[2] is execution buffer
 *
 * This is the type of the per-platform huc copy functions.
 *
 * The huc copy function emits a batchbuffer to the VDBOX engine to
 * invoke the HuC Copy kernel to copy 4K bytes from the source buffer
 * to the destination buffer.
 */
typedef void (*igt_huc_copyfunc_t)(int fd,
		struct drm_i915_gem_exec_object2 *obj);

igt_huc_copyfunc_t	igt_get_huc_copyfunc(int devid);
#endif
