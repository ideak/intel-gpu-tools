// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "igt.h"
#include "igt_vgem.h"
#include "sw_sync.h"

#include "dmabuf_sync_file.h"

/**
 * SECTION: dmabuf_sync_file
 * @short_description: DMABUF importing/exporting fencing support library
 * @title: DMABUF Sync File
 * @include: dmabuf_sync_file.h
 */

struct igt_dma_buf_sync_file {
	__u32 flags;
	__s32 fd;
};

#define IGT_DMA_BUF_IOCTL_EXPORT_SYNC_FILE _IOWR(DMA_BUF_BASE, 2, struct igt_dma_buf_sync_file)
#define IGT_DMA_BUF_IOCTL_IMPORT_SYNC_FILE _IOW(DMA_BUF_BASE, 3, struct igt_dma_buf_sync_file)

/**
 * has_dmabuf_export_sync_file:
 * @fd: The open drm fd
 *
 * Check if the kernel supports exporting a sync file from dmabuf.
 */
bool has_dmabuf_export_sync_file(int fd)
{
	struct vgem_bo bo;
	int dmabuf, ret;
	struct igt_dma_buf_sync_file arg;

	bo.width = 1;
	bo.height = 1;
	bo.bpp = 32;
	vgem_create(fd, &bo);

	dmabuf = prime_handle_to_fd(fd, bo.handle);
	gem_close(fd, bo.handle);

	arg.flags = DMA_BUF_SYNC_WRITE;
	arg.fd = -1;

	ret = igt_ioctl(dmabuf, IGT_DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &arg);
	close(dmabuf);

	return (ret == 0 || errno == ENOTTY);
}

/**
 * dmabuf_export_sync_file:
 * @dmabuf: The dmabuf fd
 * @flags: The flags to control the behaviour
 *
 * Take a snapshot of the current dma-resv fences in the dmabuf, and export as a
 * syncfile. The @flags should at least specify either DMA_BUF_SYNC_WRITE or
 * DMA_BUF_SYNC_READ, depending on if we care about the read or write fences.
 */
int dmabuf_export_sync_file(int dmabuf, uint32_t flags)
{
	struct igt_dma_buf_sync_file arg;

	arg.flags = flags;
	arg.fd = -1;
	do_ioctl(dmabuf, IGT_DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &arg);

	return arg.fd;
}

/**
 * has_dmabuf_import_sync_file:
 * @fd: The open drm fd
 *
 * Check if the kernel supports importing a sync file into a dmabuf.
 */
bool has_dmabuf_import_sync_file(int fd)
{
	struct vgem_bo bo;
	int dmabuf, timeline, fence, ret;
	struct igt_dma_buf_sync_file arg;

	bo.width = 1;
	bo.height = 1;
	bo.bpp = 32;
	vgem_create(fd, &bo);

	dmabuf = prime_handle_to_fd(fd, bo.handle);
	gem_close(fd, bo.handle);

	timeline = sw_sync_timeline_create();
	fence = sw_sync_timeline_create_fence(timeline, 1);
	sw_sync_timeline_inc(timeline, 1);

	arg.flags = DMA_BUF_SYNC_RW;
	arg.fd = fence;

	ret = igt_ioctl(dmabuf, IGT_DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &arg);
	close(dmabuf);
	close(fence);
	return (ret == 0 || errno == ENOTTY);
}

/**
 * dmabuf_import_sync_file:
 * @dmabuf: The dmabuf fd
 * @flags: The flags to control the behaviour
 * @sync_fd: The sync file (i.e our fence) to import
 *
 * Import the sync file @sync_fd, into the dmabuf. The @flags should at least
 * specify DMA_BUF_SYNC_WRITE or DMA_BUF_SYNC_READ, depending on if we are
 * importing the @sync_fd as a read or write fence.
 */
void dmabuf_import_sync_file(int dmabuf, uint32_t flags, int sync_fd)
{
	struct igt_dma_buf_sync_file arg;

	arg.flags = flags;
	arg.fd = sync_fd;
	do_ioctl(dmabuf, IGT_DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &arg);
}

/**
 * dmabuf_import_timeline_fence:
 * @dmabuf: The dmabuf fd
 * @flags: The flags to control the behaviour
 * @timeline: The sync file timeline where the new fence is created
 * @seqno: The sequence number to use for the fence
 *
 * Create a new fence as part of @timeline, and import as a sync file into the
 * dmabuf.  The @flags should at least specify DMA_BUF_SYNC_WRITE or
 * DMA_BUF_SYNC_READ, depending on if we are importing the new fence as a read
 * or write fence.
 */
void dmabuf_import_timeline_fence(int dmabuf, uint32_t flags,
				  int timeline, uint32_t seqno)
{
	int fence;

	fence = sw_sync_timeline_create_fence(timeline, seqno);
	dmabuf_import_sync_file(dmabuf, flags, fence);
	close(fence);
}

/**
 * dmabuf_busy:
 * @dmabuf: The dmabuf fd
 * @flags: The flags to control the behaviour
 *
 * Check if the fences in the dmabuf are still busy. The @flags should at least
 * specify DMA_BUF_SYNC_WRITE or DMA_BUF_SYNC_READ, depending on if we are
 * checking if the read or read fences have all signalled. Or DMA_BUF_SYNC_RW if
 * we care about both.
 */
bool dmabuf_busy(int dmabuf, uint32_t flags)
{
	struct pollfd pfd = { .fd = dmabuf };

	/* If DMA_BUF_SYNC_WRITE is set, we don't want to set POLLIN or
	 * else poll() may return a non-zero value if there are only read
	 * fences because POLLIN is ready even if POLLOUT isn't.
	 */
	if (flags & DMA_BUF_SYNC_WRITE)
		pfd.events |= POLLOUT;
	else if (flags & DMA_BUF_SYNC_READ)
		pfd.events |= POLLIN;

	return poll(&pfd, 1, 0) == 0;
}

/**
 * sync_file_busy:
 * @sync_file: The sync file to check
 *
 * Check if the @sync_file is still busy or not.
 */
bool sync_file_busy(int sync_file)
{
	struct pollfd pfd = { .fd = sync_file, .events = POLLIN };
	return poll(&pfd, 1, 0) == 0;
}

/**
 * dmabuf_sync_file_busy:
 * @dmabuf: The dmabuf fd
 * @flags: The flags to control the behaviour
 *
 * Export the current fences in @dmabuf as a sync file and check if still busy.
 * The @flags should at least contain DMA_BUF_SYNC_WRITE or DMA_BUF_SYNC_READ,
 * to specify which fences are to be exported from the @dmabuf and checked if
 * busy. Or DMA_BUF_SYNC_RW if we care about both.
 */
bool dmabuf_sync_file_busy(int dmabuf, uint32_t flags)
{
	int sync_file;
	bool busy;

	sync_file = dmabuf_export_sync_file(dmabuf, flags);
	busy = sync_file_busy(sync_file);
	close(sync_file);

	return busy;
}
