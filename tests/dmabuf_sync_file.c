// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "igt.h"
#include "igt_vgem.h"
#include "sw_sync.h"
#include "dmabuf_sync_file.h"

IGT_TEST_DESCRIPTION("Tests for sync_file support in dma-buf");

static void test_export_basic(int fd)
{
	struct vgem_bo bo;
	int dmabuf;
	uint32_t fence;

	igt_require(has_dmabuf_export_sync_file(fd));

	bo.width = 1;
	bo.height = 1;
	bo.bpp = 32;
	vgem_create(fd, &bo);

	dmabuf = prime_handle_to_fd(fd, bo.handle);

	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_WRITE));

	fence = vgem_fence_attach(fd, &bo, 0);
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(dmabuf_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(dmabuf_busy(dmabuf, DMA_BUF_SYNC_RW));
	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_RW));

	vgem_fence_signal(fd, fence);
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_RW));
	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_RW));

	fence = vgem_fence_attach(fd, &bo, VGEM_FENCE_WRITE);
	igt_assert(dmabuf_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(dmabuf_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(dmabuf_busy(dmabuf, DMA_BUF_SYNC_RW));
	igt_assert(dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_RW));

	vgem_fence_signal(fd, fence);
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_RW));
	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_RW));

	close(dmabuf);
	gem_close(fd, bo.handle);
}

static void test_export_before_signal(int fd)
{
	struct vgem_bo bo;
	int dmabuf, read_fd, write_fd;
	uint32_t fence;

	igt_require(has_dmabuf_export_sync_file(fd));

	bo.width = 1;
	bo.height = 1;
	bo.bpp = 32;
	vgem_create(fd, &bo);

	dmabuf = prime_handle_to_fd(fd, bo.handle);

	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_WRITE));

	fence = vgem_fence_attach(fd, &bo, 0);

	read_fd = dmabuf_export_sync_file(dmabuf, DMA_BUF_SYNC_READ);
	write_fd = dmabuf_export_sync_file(dmabuf, DMA_BUF_SYNC_WRITE);

	igt_assert(!sync_file_busy(read_fd));
	igt_assert(sync_file_busy(write_fd));

	vgem_fence_signal(fd, fence);

	igt_assert(!sync_file_busy(read_fd));
	igt_assert(!sync_file_busy(write_fd));

	close(read_fd);
	close(write_fd);

	fence = vgem_fence_attach(fd, &bo, VGEM_FENCE_WRITE);

	read_fd = dmabuf_export_sync_file(dmabuf, DMA_BUF_SYNC_READ);
	write_fd = dmabuf_export_sync_file(dmabuf, DMA_BUF_SYNC_WRITE);

	igt_assert(sync_file_busy(read_fd));
	igt_assert(sync_file_busy(write_fd));

	vgem_fence_signal(fd, fence);

	igt_assert(!sync_file_busy(read_fd));
	igt_assert(!sync_file_busy(write_fd));

	close(read_fd);
	close(write_fd);

	close(dmabuf);
	gem_close(fd, bo.handle);
}

static void test_export_multiwait(int fd)
{
	struct vgem_bo bo;
	int dmabuf, sync_file;
	uint32_t fence1, fence2, fence3;

	igt_require(has_dmabuf_export_sync_file(fd));

	bo.width = 1;
	bo.height = 1;
	bo.bpp = 32;
	vgem_create(fd, &bo);

	dmabuf = prime_handle_to_fd(fd, bo.handle);

	fence1 = vgem_fence_attach(fd, &bo, 0);
	fence2 = vgem_fence_attach(fd, &bo, 0);

	sync_file = dmabuf_export_sync_file(dmabuf, DMA_BUF_SYNC_WRITE);

	fence3 = vgem_fence_attach(fd, &bo, 0);

	igt_assert(sync_file_busy(sync_file));

	vgem_fence_signal(fd, fence1);

	igt_assert(sync_file_busy(sync_file));

	vgem_fence_signal(fd, fence2);

	igt_assert(!sync_file_busy(sync_file));

	vgem_fence_signal(fd, fence3);

	close(sync_file);
	close(dmabuf);
	gem_close(fd, bo.handle);
}

static void test_export_wait_after_attach(int fd)
{
	struct vgem_bo bo;
	int dmabuf, read_sync_file, write_sync_file;
	uint32_t fence1, fence2;

	igt_require(has_dmabuf_export_sync_file(fd));

	bo.width = 1;
	bo.height = 1;
	bo.bpp = 32;
	vgem_create(fd, &bo);

	dmabuf = prime_handle_to_fd(fd, bo.handle);

	read_sync_file = dmabuf_export_sync_file(dmabuf, DMA_BUF_SYNC_READ);
	write_sync_file = dmabuf_export_sync_file(dmabuf, DMA_BUF_SYNC_WRITE);

	fence1 = vgem_fence_attach(fd, &bo, VGEM_FENCE_WRITE);

	igt_assert(!sync_file_busy(read_sync_file));
	igt_assert(!sync_file_busy(write_sync_file));
	close(read_sync_file);
	close(write_sync_file);

	/* These wait on fence1 */
	read_sync_file = dmabuf_export_sync_file(dmabuf, DMA_BUF_SYNC_READ);
	write_sync_file = dmabuf_export_sync_file(dmabuf, DMA_BUF_SYNC_WRITE);

	igt_assert(sync_file_busy(read_sync_file));
	igt_assert(sync_file_busy(write_sync_file));

	vgem_fence_signal(fd, fence1);
	fence2 = vgem_fence_attach(fd, &bo, VGEM_FENCE_WRITE);

	/* fence1 has signaled */
	igt_assert(!sync_file_busy(read_sync_file));
	igt_assert(!sync_file_busy(write_sync_file));

	/* fence2 has not */
	igt_assert(dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_WRITE));

	vgem_fence_signal(fd, fence2);
	close(read_sync_file);
	close(write_sync_file);

	close(dmabuf);
	gem_close(fd, bo.handle);
}

static void test_import_basic(int fd)
{
	struct vgem_bo bo;
	int dmabuf, timeline;

	igt_require_sw_sync();
	igt_require(has_dmabuf_import_sync_file(fd));

	bo.width = 1;
	bo.height = 1;
	bo.bpp = 32;
	vgem_create(fd, &bo);

	dmabuf = prime_handle_to_fd(fd, bo.handle);

	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_WRITE));

	timeline = sw_sync_timeline_create();

	dmabuf_import_timeline_fence(dmabuf, DMA_BUF_SYNC_READ, timeline, 1);
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(dmabuf_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(dmabuf_busy(dmabuf, DMA_BUF_SYNC_RW));
	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_RW));

	sw_sync_timeline_inc(timeline, 1);
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_RW));
	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_RW));

	dmabuf_import_timeline_fence(dmabuf, DMA_BUF_SYNC_WRITE, timeline, 2);
	igt_assert(dmabuf_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(dmabuf_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(dmabuf_busy(dmabuf, DMA_BUF_SYNC_RW));
	igt_assert(dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_RW));

	sw_sync_timeline_inc(timeline, 1);
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_RW));
	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_RW));

	dmabuf_import_timeline_fence(dmabuf, DMA_BUF_SYNC_RW, timeline, 3);
	igt_assert(dmabuf_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(dmabuf_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(dmabuf_busy(dmabuf, DMA_BUF_SYNC_RW));
	igt_assert(dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_RW));

	sw_sync_timeline_inc(timeline, 1);
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_RW));
	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(!dmabuf_sync_file_busy(dmabuf, DMA_BUF_SYNC_RW));
}

static void test_import_multiple(int fd, bool write)
{
	struct vgem_bo bo;
	int i, dmabuf, read_sync_file, write_sync_file;
	int write_timeline = -1, read_timelines[32];

	igt_require_sw_sync();
	igt_require(has_dmabuf_import_sync_file(fd));

	bo.width = 1;
	bo.height = 1;
	bo.bpp = 32;
	vgem_create(fd, &bo);

	dmabuf = prime_handle_to_fd(fd, bo.handle);

	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_WRITE));

	for (i = 0; i < ARRAY_SIZE(read_timelines); i++) {
		read_timelines[i] = sw_sync_timeline_create();
		dmabuf_import_timeline_fence(dmabuf, DMA_BUF_SYNC_READ,
					     read_timelines[i], 1);
	}

	if (write) {
		write_timeline = sw_sync_timeline_create();
		dmabuf_import_timeline_fence(dmabuf, DMA_BUF_SYNC_WRITE,
					     write_timeline, 1);
	}

	read_sync_file = dmabuf_export_sync_file(dmabuf, DMA_BUF_SYNC_READ);
	write_sync_file = dmabuf_export_sync_file(dmabuf, DMA_BUF_SYNC_WRITE);

	for (i = ARRAY_SIZE(read_timelines) - 1; i >= 0; i--) {
		igt_assert_eq(dmabuf_busy(dmabuf, DMA_BUF_SYNC_READ), write);
		igt_assert_eq(sync_file_busy(read_sync_file), write);
		igt_assert(dmabuf_busy(dmabuf, DMA_BUF_SYNC_WRITE));
		igt_assert(sync_file_busy(write_sync_file));

		sw_sync_timeline_inc(read_timelines[i], 1);
	}

	igt_assert_eq(dmabuf_busy(dmabuf, DMA_BUF_SYNC_READ), write);
	igt_assert_eq(sync_file_busy(read_sync_file), write);
	igt_assert_eq(dmabuf_busy(dmabuf, DMA_BUF_SYNC_WRITE), write);
	igt_assert_eq(sync_file_busy(write_sync_file), write);

	if (write)
		sw_sync_timeline_inc(write_timeline, 1);

	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_READ));
	igt_assert(!sync_file_busy(read_sync_file));
	igt_assert(!dmabuf_busy(dmabuf, DMA_BUF_SYNC_WRITE));
	igt_assert(!sync_file_busy(write_sync_file));
}

igt_main
{
	int fd;

	igt_fixture {
		fd = drm_open_driver(DRIVER_VGEM);
	}

	igt_describe("Sanity test for exporting a sync_file from a dma-buf.");
	igt_subtest("export-basic")
		test_export_basic(fd);

	igt_describe("Test exporting a sync_file from a dma-buf before "
		     "signaling any of its fences.");
	igt_subtest("export-before-signal")
		test_export_before_signal(fd);

	igt_describe("Test exporting a sync_file from a dma-buf with multiple "
		     "fences on it.");
	igt_subtest("export-multiwait")
		test_export_multiwait(fd);

	igt_describe("Test exporting a sync_file from a dma-buf then adding "
		     "fences to the dma-buf before we wait.  The sync_file "
		     "should snapshot the current set of fences and not wait "
		     "for any fences added after it was exported.");
	igt_subtest("export-wait-after-attach")
		test_export_wait_after_attach(fd);

	igt_describe("Sanity test for importing a sync_file into a dma-buf.");
	igt_subtest("import-basic")
		test_import_basic(fd);

	igt_describe("Test importing multiple read-only fences into a dma-buf. "
		     "They should all block any write operations but not other "
		     "read operations.");
	igt_subtest("import-multiple-read-only")
		test_import_multiple(fd, false);

	igt_describe("Test importing multiple read-write fences into a "
		     "dma-buf. They should all block any read or write "
		     "operations.");
	igt_subtest("import-multiple-read-write")
		test_import_multiple(fd, true);
}
