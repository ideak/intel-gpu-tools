/*
 * Copyright 2021 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef IGT_NOUVEAU_H
#define IGT_NOUVEAU_H

#include <stddef.h>
#include <inttypes.h>

#include <nouveau/nouveau.h>
#include <nouveau/nvif/class.h>
#include <nouveau/nvif/cl0080.h>

#include "igt_core.h"

#define IGT_NOUVEAU_CHIPSET_GV100 0x140

typedef struct igt_fb igt_fb_t;

#ifdef HAVE_LIBDRM_NOUVEAU
#define DECL(d) d
#else
/* There shouldn't be any code that calls igt_nouveau_* functions without libdrm support enabled, as
 * is_nouveau_device() always returns false with libdrm support disabled. We still need to provide
 * function definitions though, since the alternative would be littering igt with ifdefs
 */
static inline __noreturn void __igt_nouveau_skip(void) {
	igt_skip("Nouveau libdrm support disabled\n");
}
#define DECL(d) static inline __noreturn d { __igt_nouveau_skip(); }
#endif

DECL(uint32_t igt_nouveau_get_chipset(int fd));
DECL(uint64_t igt_nouveau_get_block_height(uint64_t modifier));

DECL(int igt_nouveau_create_bo(int drm_fd, bool sysmem, igt_fb_t *fb));
DECL(void igt_nouveau_delete_bo(igt_fb_t *fb));
DECL(void *igt_nouveau_mmap_bo(igt_fb_t *fb, int prot));
DECL(void igt_nouveau_munmap_bo(igt_fb_t *fb));
DECL(bool igt_nouveau_is_tiled(uint64_t modifier));

DECL(void igt_nouveau_fb_clear(struct igt_fb *fb));
DECL(void igt_nouveau_fb_blit(struct igt_fb *dst, struct igt_fb *src));

#undef DECL
#endif /* !IGT_NOUVEAU_H */
