#include "intel_batchbuffer.h"


static inline void emit_vertex_2s(struct intel_bb *ibb,
				  int16_t x, int16_t y)
{
	intel_bb_out(ibb, (uint32_t) y << 16 | (uint32_t) x);
}

static inline void emit_vertex(struct intel_bb *ibb,
			       float f)
{
	union { float f; uint32_t ui; } u;
	u.f = f;
	intel_bb_out(ibb, u.ui);
}

static inline void emit_vertex_normalized(struct intel_bb *ibb,
					  float f, float total)
{
	union { float f; uint32_t ui; } u;
	u.f = f / total;
	intel_bb_out(ibb, u.ui);
}

void gen12_render_clearfunc(struct intel_bb *ibb,
			    struct intel_buf *dst, unsigned int dst_x, unsigned int dst_y,
			    unsigned int width, unsigned int height,
			    const float clear_color[4]);
void gen12_render_copyfunc(struct intel_bb *ibb,
			   struct intel_buf *src, uint32_t src_x, uint32_t src_y,
			   uint32_t width, uint32_t height,
			   struct intel_buf *dst, uint32_t dst_x, uint32_t dst_y);
void gen11_render_copyfunc(struct intel_bb *ibb,
			  struct intel_buf *src, uint32_t src_x, uint32_t src_y,
			  uint32_t width, uint32_t height,
			  struct intel_buf *dst, uint32_t dst_x, uint32_t dst_y);
void gen9_render_copyfunc(struct intel_bb *ibb,
			  struct intel_buf *src, uint32_t src_x, uint32_t src_y,
			  uint32_t width, uint32_t height,
			  struct intel_buf *dst, uint32_t dst_x, uint32_t dst_y);
void gen8_render_copyfunc(struct intel_bb *ibb,
			  struct intel_buf *src, uint32_t src_x, uint32_t src_y,
			  uint32_t width, uint32_t height,
			  struct intel_buf *dst, uint32_t dst_x, uint32_t dst_y);
void gen7_render_copyfunc(struct intel_bb *ibb,
			  struct intel_buf *src, uint32_t src_x, uint32_t src_y,
			  uint32_t width, uint32_t height,
			  struct intel_buf *dst, uint32_t dst_x, uint32_t dst_y);
void gen6_render_copyfunc(struct intel_bb *ibb,
			  struct intel_buf *src, uint32_t src_x, uint32_t src_y,
			  uint32_t width, uint32_t height,
			  struct intel_buf *dst, uint32_t dst_x, uint32_t dst_y);
void gen4_render_copyfunc(struct intel_bb *ibb,
			  struct intel_buf *src, uint32_t src_x, uint32_t src_y,
			  uint32_t width, uint32_t height,
			  struct intel_buf *dst, uint32_t dst_x, uint32_t dst_y);
void gen3_render_copyfunc(struct intel_bb *ibb,
			  struct intel_buf *src, uint32_t src_x, uint32_t src_y,
			  uint32_t width, uint32_t height,
			  struct intel_buf *dst, uint32_t dst_x, uint32_t dst_y);
void gen2_render_copyfunc(struct intel_bb *ibb,
			  struct intel_buf *src, uint32_t src_x, uint32_t src_y,
			  uint32_t width, uint32_t height,
			  struct intel_buf *dst, uint32_t dst_x, uint32_t dst_y);
