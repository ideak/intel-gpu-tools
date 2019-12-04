#ifndef __VEBOXCOPY_H__
#define __VEBOXCOPY_H__

void gen12_vebox_copyfunc(struct intel_batchbuffer *batch,
			  const struct igt_buf *src,
			  unsigned width, unsigned height,
			  const struct igt_buf *dst);

#endif
