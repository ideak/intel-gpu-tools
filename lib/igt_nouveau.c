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

#include <stdlib.h>
#include <inttypes.h>

#include <nouveau_drm.h>
#include <nouveau/nouveau.h>
#include <nouveau/nvif/class.h>

#include "igt.h"
#include "igt_list.h"
#include "igt_nouveau.h"

#include "nouveau/nvif/push.h"
#include "nouveau/nvhw/class/cla0b5.h"
#include "nouveau/priv.h"

#define PASCAL_DMA_COPY_A                                                            (0x0000C0B5)
#define PASCAL_DMA_COPY_B                                                            (0x0000C1B5)
#define VOLTA_DMA_COPY_A                                                             (0x0000C3B5)
#define TURING_DMA_COPY_A                                                            (0x0000C5B5)
#define AMPERE_DMA_COPY_A                                                            (0x0000C6B5)

struct igt_nouveau_fb_priv {
	struct igt_nouveau_dev *dev;
	struct nouveau_bo *bo;
};

static struct igt_nouveau_dev *get_nouveau_dev(int drm_fd)
{
	struct igt_nouveau_dev *dev;
	struct nouveau_drm *drm;
	static IGT_LIST_HEAD(devices);

	igt_list_for_each_entry(dev, &devices, node) {
		if (dev->drm->fd == drm_fd)
			return dev;
	}

	igt_assert(dev = malloc(sizeof(*dev)));
	memset(dev, 0, sizeof(*dev));

	IGT_INIT_LIST_HEAD(&dev->node);

	do_or_die(nouveau_drm_new(drm_fd, &dev->drm));
	drm = dev->drm;

	igt_skip_on_f(!drm->nvif, "Only the NVIF interface for nouveau is supported\n");

	do_or_die(nouveau_device_new(&drm->client, NV_DEVICE,
				     &(struct nv_device_v0) { .device = ~0ULL, },
				     sizeof(struct nv_device_v0), &dev->dev));
	do_or_die(nouveau_client_new(dev->dev, &dev->client));

	igt_list_add(&dev->node, &devices);

	return dev;
}

uint32_t igt_nouveau_get_chipset(int fd)
{
	return get_nouveau_dev(fd)->dev->chipset;
}

uint64_t igt_nouveau_get_block_height(uint64_t modifier)
{
	uint8_t gob_height;
	uint8_t log_block_height_in_gobs = (modifier & 0xF);

	switch ((modifier >> 20) & 0x3) {
	case 0:
	case 2:
		gob_height = 8;
		break;
	case 1:
		gob_height = 4;
		break;
	default:
		igt_fail_on_f(true, "Unknown GOB height/page kind generation 3 in modifier %lx\n",
			      modifier);
		break;
	}

	return gob_height * (1 << log_block_height_in_gobs);
}

static void
decode_mod(uint16_t chipset, uint64_t modifier, uint32_t *tile_mode, uint32_t *kind)
{
	*tile_mode = modifier & 0xF;
	*kind = (modifier >> 12) & 0xFF;

	if (chipset >= 0xc0)
		*tile_mode <<= 4;
}

int igt_nouveau_create_bo(int drm_fd, bool sysmem, igt_fb_t *fb)
{
	struct igt_nouveau_dev *dev = get_nouveau_dev(drm_fd);
	struct nouveau_device *nvdev = dev->dev;
	union nouveau_bo_config config = { };
	struct igt_nouveau_fb_priv *priv;
	uint32_t flags = sysmem ? NOUVEAU_BO_GART : NOUVEAU_BO_VRAM;

	if (fb->modifier)
		decode_mod(nvdev->chipset, fb->modifier,
			   &config.nvc0.tile_mode, &config.nvc0.memtype);

	igt_assert(priv = malloc(sizeof(*priv)));
	do_or_die(nouveau_bo_new(nvdev, flags | NOUVEAU_BO_RDWR, nvdev->chipset < 0x140 ? 256 : 64,
				 fb->size, &config, &priv->bo));
	priv->dev = dev;
	fb->driver_priv = priv;

	if (!sysmem)
		igt_nouveau_fb_clear(fb);

	return priv->bo->handle;
}

void *igt_nouveau_mmap_bo(igt_fb_t *fb, int prot)
{
	struct igt_nouveau_fb_priv *priv = fb->driver_priv;
	struct igt_nouveau_dev *dev = priv->dev;
	struct nouveau_client *client = dev->client;

	do_or_die(nouveau_bo_map(priv->bo, prot, client));

	return priv->bo->map;
}

void igt_nouveau_munmap_bo(igt_fb_t *fb)
{
	struct igt_nouveau_fb_priv *priv = fb->driver_priv;

	munmap(priv->bo->map, priv->bo->size);
	priv->bo->map = NULL;
}

void igt_nouveau_delete_bo(igt_fb_t *fb)
{
	struct igt_nouveau_fb_priv *priv = fb->driver_priv;

	nouveau_bo_ref(NULL, &priv->bo);
	free(priv);
}

bool igt_nouveau_is_tiled(uint64_t modifier)
{
	switch (modifier) {
	case DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(0):
	case DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(1):
	case DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(2):
	case DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(3):
	case DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(4):
	case DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(5):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x7a, 0):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x7a, 1):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x7a, 2):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x7a, 3):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x7a, 4):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x7a, 5):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x78, 0):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x78, 1):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x78, 2):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x78, 3):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x78, 4):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x78, 5):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x70, 0):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x70, 1):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x70, 2):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x70, 3):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x70, 4):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x70, 5):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 0, 0xfe, 0):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 0, 0xfe, 1):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 0, 0xfe, 2):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 0, 0xfe, 3):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 0, 0xfe, 4):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 0, 0xfe, 5):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 0x06, 0):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 0x06, 1):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 0x06, 2):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 0x06, 3):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 0x06, 4):
	case DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 2, 0x06, 5):
		return true;
	default:
		return false;
	}
}

/* TODO: Implement CE on Fermi */
static void init_ce(struct igt_nouveau_dev *dev)
{
	struct nouveau_device *nv_dev = dev->dev;
	struct nouveau_client *client = dev->client;
	struct nouveau_mclass mclass[] = {
		{ AMPERE_DMA_COPY_A,  -1, NULL },
		{ TURING_DMA_COPY_A,  -1, NULL },
		{ VOLTA_DMA_COPY_A,   -1, NULL },
		{ PASCAL_DMA_COPY_B,  -1, NULL },
		{ PASCAL_DMA_COPY_A,  -1, NULL },
		{ MAXWELL_DMA_COPY_A, -1, NULL },
		{ KEPLER_DMA_COPY_A,  -1, NULL },
		{ 0 }
	};
	int oclass_idx;
	uint32_t oclass;

	if (dev->ce)
		return;

	do_or_die(nouveau_object_new(&nv_dev->object, 0, NOUVEAU_FIFO_CHANNEL_CLASS,
				     &(struct nve0_fifo) {
					     .engine = NVE0_FIFO_ENGINE_CE0 | NVE0_FIFO_ENGINE_CE1,
				     }, sizeof(struct nve0_fifo), &dev->ce_channel));

	oclass_idx = nouveau_object_mclass(dev->ce_channel, mclass);
	igt_assert_f(oclass_idx >= 0, "No supported dma-copy classes found\n");
	oclass = mclass[oclass_idx].oclass;
	igt_debug("Found dma-copy class %04x\n", oclass);

	do_or_die(nouveau_pushbuf_new(client, dev->ce_channel, 4, 32 * 1024, 1, &dev->pushbuf));
	do_or_die(nouveau_object_new(dev->ce_channel, oclass, oclass, NULL, 0, &dev->ce));
}

void igt_nouveau_fb_clear(struct igt_fb *fb)
{
	struct igt_nouveau_fb_priv *priv = fb->driver_priv;
	struct igt_nouveau_dev *dev = priv->dev;

	init_ce(dev);

	igt_set_timeout(30, "Timed out while clearing bo with dma-copy");

	for (unsigned int plane = 0; plane < fb->num_planes; plane++)
		igt_nouveau_ce_zfilla0b5(dev, fb, priv->bo, plane);

	do_or_die(nouveau_bo_wait(priv->bo, NOUVEAU_BO_RD, dev->client));

	igt_reset_timeout();
}

void igt_nouveau_fb_blit(struct igt_fb *dst, struct igt_fb *src)
{
	struct igt_nouveau_fb_priv *dst_priv = dst->driver_priv, *src_priv = src->driver_priv;
	struct igt_nouveau_dev *dev = dst_priv->dev;
	struct nouveau_bo *dst_nvbo = dst_priv->bo, *src_nvbo = src_priv->bo;

	init_ce(dev);

	igt_set_timeout(30, "Timed out while blitting bo with dma-copy");

	for (unsigned int plane = 0; plane < src->num_planes; plane++)
		igt_nouveau_ce_copya0b5(dev, dst, dst_nvbo, src, src_nvbo, plane);

	do_or_die(nouveau_bo_wait(dst_priv->bo, NOUVEAU_BO_RD, dev->client));

	igt_reset_timeout();
}
