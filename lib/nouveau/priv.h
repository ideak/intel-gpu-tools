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

#ifndef _IGT_NOUVEAU_PRIV_H_
#define _IGT_NOUVEAU_PRIV_H_

#include <inttypes.h>
#include <pthread.h>

#include <nouveau_drm.h>
#include <nouveau/nouveau.h>
#include <nouveau/nvif/class.h>
#include <nouveau/nvif/cl0080.h>

#include "igt_list.h"

struct igt_fb;

struct igt_nouveau_dev {
	struct nouveau_drm *drm;
	struct nouveau_device *dev;
	struct nouveau_client *client;

	struct nouveau_object *ce_channel;
	struct nouveau_object *ce;
	struct nouveau_pushbuf *pushbuf;

	struct igt_list_head node;
};

void igt_nouveau_ce_zfilla0b5(struct igt_nouveau_dev *dev, struct igt_fb *fb, struct nouveau_bo *bo,
			      unsigned int plane);
void igt_nouveau_ce_copya0b5(struct igt_nouveau_dev *dev,
			     struct igt_fb *dst_fb, struct nouveau_bo *dst_bo,
			     struct igt_fb *src_fb, struct nouveau_bo *src_bo,
			     unsigned int plane);

#endif /* !_IGT_NOUVEAU_PRIV_H_ */
