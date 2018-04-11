#ifndef RENDE_MEDIA_FILL_H
#define RENDE_MEDIA_FILL_H

#include <stdint.h>
#include "intel_batchbuffer.h"

void
gen8_media_fillfunc(struct intel_batchbuffer *batch,
		struct igt_buf *dst,
		unsigned int x, unsigned int y,
		unsigned int width, unsigned int height,
		uint8_t color);

void
gen7_media_fillfunc(struct intel_batchbuffer *batch,
		struct igt_buf *dst,
		unsigned int x, unsigned int y,
		unsigned int width, unsigned int height,
		uint8_t color);

void
gen9_media_fillfunc(struct intel_batchbuffer *batch,
		struct igt_buf *dst,
		unsigned int x, unsigned int y,
		unsigned int width, unsigned int height,
		uint8_t color);

#endif /* RENDE_MEDIA_FILL_H */
