#ifndef __VEBOXCOPY_H__
#define __VEBOXCOPY_H__

void gen12_vebox_copyfunc(struct intel_bb *ibb,
			  struct intel_buf *src,
			  unsigned int width, unsigned int height,
			  struct intel_buf *dst);

#endif
