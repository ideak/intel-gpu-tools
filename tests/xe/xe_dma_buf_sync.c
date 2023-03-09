// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

/**
 * TEST: Check dmabuf functionality
 * Category: Software building block
 * Sub-category: dmabuf
 * Test category: functionality test
 */

#include "igt.h"
#include "lib/igt_syncobj.h"
#include "lib/intel_reg.h"
#include "xe_drm.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe/xe_spin.h"
#include <string.h>
#include <linux/dma-buf.h>
#include <sys/poll.h>

#define MAX_N_BO	16
#define N_FD		2

#define READ_SYNC	(0x1 << 0)

struct igt_dma_buf_sync_file {
	__u32 flags;
	__s32 fd;
};

#define IGT_DMA_BUF_IOCTL_EXPORT_SYNC_FILE \
	_IOWR(DMA_BUF_BASE, 2, struct igt_dma_buf_sync_file)

static int dmabuf_export_sync_file(int dmabuf, uint32_t flags)
{
	struct igt_dma_buf_sync_file arg;

	arg.flags = flags;
	arg.fd = -1;
	do_ioctl(dmabuf, IGT_DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &arg);

	return arg.fd;
}

static bool dmabuf_busy(int dmabuf, uint32_t flags)
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

static bool sync_file_busy(int sync_file)
{
	struct pollfd pfd = { .fd = sync_file, .events = POLLIN };
	return poll(&pfd, 1, 0) == 0;
}

/**
 * SUBTEST: export-dma-buf-once
 * Description: Test exporting a sync file from a dma-buf
 * Run type: BAT
 *
 * SUBTEST: export-dma-buf-once-read-sync
 * Description: Test export prime BO as sync file and verify business
 * Run type: BAT
 *
 * SUBTEST: export-dma-buf-many
 * Description: Test exporting many sync files from a dma-buf
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 *
 * SUBTEST: export-dma-buf-many-read-sync
 * Description: Test export many prime BO as sync file and verify business
 * Run type: FULL
 * TODO: change ``'Run type' == FULL`` to a better category
 */

static void
test_export_dma_buf(struct drm_xe_engine_class_instance *hwe0,
		    struct drm_xe_engine_class_instance *hwe1,
		    int n_bo, int flags)
{
	uint64_t addr = 0x1a0000, base_addr = 0x1a0000;
	int fd[N_FD];
	uint32_t bo[MAX_N_BO];
	int dma_buf_fd[MAX_N_BO];
	uint32_t import_bo[MAX_N_BO];
	uint32_t vm[N_FD];
	uint32_t engine[N_FD];
	size_t bo_size;
	struct {
		struct xe_spin spin;
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data [MAX_N_BO];
	int i;

	igt_assert(n_bo <= MAX_N_BO);

	for (i = 0; i < N_FD; ++i) {
		fd[i] = drm_open_driver(DRIVER_XE);
		xe_device_get(fd[0]);
		vm[i] = xe_vm_create(fd[i], 0, 0);
		engine[i] = xe_engine_create(fd[i], vm[i], !i ? hwe0 : hwe1, 0);
	}

	bo_size = sizeof(*data[0]) * N_FD;
	bo_size = ALIGN(bo_size + xe_cs_prefetch_size(fd[0]),
			xe_get_default_alignment(fd[0]));
	for (i = 0; i < n_bo; ++i) {
		bo[i] = xe_bo_create(fd[0], hwe0->gt_id, 0, bo_size);
		dma_buf_fd[i] = prime_handle_to_fd(fd[0], bo[i]);
		import_bo[i] = prime_fd_to_handle(fd[1], dma_buf_fd[i]);

		if (i & 1)
			data[i] = xe_bo_map(fd[1], import_bo[i], bo_size);
		else
			data[i] = xe_bo_map(fd[0], bo[i], bo_size);
		memset(data[i], 0, bo_size);

		xe_vm_bind_sync(fd[0], vm[0], bo[i], 0, addr, bo_size);
		xe_vm_bind_sync(fd[1], vm[1], import_bo[i], 0, addr, bo_size);
		addr += bo_size;
	}
	addr = base_addr;

	for (i = 0; i < n_bo; ++i) {
		uint64_t batch_offset = (char *)&data[i]->batch -
			(char *)data[i];
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data[i]->data - (char *)data[i];
		uint64_t sdi_addr = addr + sdi_offset;
		uint64_t spin_offset = (char *)&data[i]->spin - (char *)data[i];
		uint64_t spin_addr = addr + spin_offset;
		struct drm_xe_sync sync[2] = {
			{ .flags = DRM_XE_SYNC_SYNCOBJ, },
			{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
		};
		struct drm_xe_exec exec = {
			.num_batch_buffer = 1,
			.syncs = to_user_pointer(&sync),
		};
		uint32_t syncobj;
		int b = 0;
		int sync_fd;

		/* Write spinner on FD[0] */
		xe_spin_init(&data[i]->spin, spin_addr, true);
		exec.engine_id = engine[0];
		exec.address = spin_addr;
		xe_exec(fd[0], &exec);

		/* Export prime BO as sync file and veify business */
		if (flags & READ_SYNC)
			sync_fd = dmabuf_export_sync_file(dma_buf_fd[i],
							  DMA_BUF_SYNC_READ);
		else
			sync_fd = dmabuf_export_sync_file(dma_buf_fd[i],
							  DMA_BUF_SYNC_WRITE);
		xe_spin_wait_started(&data[i]->spin);
		igt_assert(sync_file_busy(sync_fd));
		igt_assert(dmabuf_busy(dma_buf_fd[i], DMA_BUF_SYNC_READ));

		/* Convert sync file to syncobj */
		syncobj = syncobj_create(fd[1], 0);
		syncobj_import_sync_file(fd[1], syncobj, sync_fd);

		/* Do an exec with syncobj as in fence on FD[1] */
		data[i]->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i]->batch[b++] = sdi_addr;
		data[i]->batch[b++] = sdi_addr >> 32;
		data[i]->batch[b++] = 0xc0ffee;
		data[i]->batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i]->batch));
		sync[0].handle = syncobj;
		sync[1].handle = syncobj_create(fd[1], 0);
		exec.engine_id = engine[1];
		exec.address = batch_addr;
		exec.num_syncs = 2;
		xe_exec(fd[1], &exec);

		/* Verify exec blocked on spinner / prime BO */
		usleep(5000);
		igt_assert(!syncobj_wait(fd[1], &sync[1].handle, 1, 1, 0,
					 NULL));
		igt_assert_eq(data[i]->data, 0x0);

		/* End spinner and verify exec complete */
		xe_spin_end(&data[i]->spin);
		igt_assert(syncobj_wait(fd[1], &sync[1].handle, 1, INT64_MAX,
					0, NULL));
		igt_assert_eq(data[i]->data, 0xc0ffee);

		/* Clean up */
		syncobj_destroy(fd[1], sync[0].handle);
		syncobj_destroy(fd[1], sync[1].handle);
		close(sync_fd);
		addr += bo_size;
	}

	for (i = 0; i < n_bo; ++i) {
		munmap(data[i], bo_size);
		gem_close(fd[0], bo[i]);
		close(dma_buf_fd[i]);
	}

	for (i = 0; i < N_FD; ++i) {
		xe_device_put(fd[i]);
		close(fd[i]);
	}

}

igt_main
{
	struct drm_xe_engine_class_instance *hwe, *hwe0 = NULL, *hwe1;
	int fd;

	igt_fixture {
		fd = drm_open_driver(DRIVER_XE);
		xe_device_get(fd);

		for_each_hw_engine(fd, hwe)
			if (hwe0 == NULL) {
				hwe0 = hwe;
			} else {
				hwe1 = hwe;
				break;
			}
	}

	igt_subtest("export-dma-buf-once")
		test_export_dma_buf(hwe0, hwe1, 1, 0);

	igt_subtest("export-dma-buf-many")
		test_export_dma_buf(hwe0, hwe1, 16, 0);

	igt_subtest("export-dma-buf-once-read-sync")
		test_export_dma_buf(hwe0, hwe1, 1, READ_SYNC);

	igt_subtest("export-dma-buf-many-read-sync")
		test_export_dma_buf(hwe0, hwe1, 16, READ_SYNC);

	igt_fixture {
		xe_device_put(fd);
		close(fd);
	}
}
