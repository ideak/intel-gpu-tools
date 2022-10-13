/* SPDX-License-Identifier: MIT
 * Copyright 2022 Advanced Micro Devices, Inc.
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
 *
*/
#include <linux/limits.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/sysmacros.h>
#include <amdgpu.h>
#include <amdgpu_drm.h>
#include "amd_PM4.h"
#include "amd_pci_unplug.h"
#include "amd_memory.h"
#include "igt.h"
#include "xalloc.h"
#include "amd_ip_blocks.h"

static int
amdgpu_open_devices(bool open_render_node, int  max_cards_supported, int drm_amdgpu_fds[])
{
	drmDevicePtr devices[MAX_CARDS_SUPPORTED];
	int i;
	int drm_node;
	int amd_index = 0;
	int drm_count;
	int fd;
	drmVersionPtr version;

	for (i = 0; i < max_cards_supported && i < MAX_CARDS_SUPPORTED; i++)
		drm_amdgpu_fds[i] = -1;

	drm_count = drmGetDevices2(0, devices, MAX_CARDS_SUPPORTED);

	if (drm_count < 0) {
		fprintf(stderr, "drmGetDevices2() returned an error %d\n", drm_count);
		return 0;
	}

	for (i = 0; i < drm_count; i++) {
		/* If this is not PCI device, skip*/
		if (devices[i]->bustype != DRM_BUS_PCI)
			continue;

		/* If this is not AMD GPU vender ID, skip*/
		if (devices[i]->deviceinfo.pci->vendor_id != 0x1002)
			continue;

		if (open_render_node)
			drm_node = DRM_NODE_RENDER;
		else
			drm_node = DRM_NODE_PRIMARY;

		fd = -1;
		if (devices[i]->available_nodes & 1 << drm_node)
			fd = open(
				devices[i]->nodes[drm_node],
				O_RDWR | O_CLOEXEC);

		/* This node is not available. */
		if (fd < 0) continue;

		version = drmGetVersion(fd);
		if (!version) {
			fprintf(stderr, "Warning: Cannot get version for %s." "Error is %s\n",
				devices[i]->nodes[drm_node], strerror(errno));
			close(fd);
			continue;
		}

		if (strcmp(version->name, "amdgpu")) {
			/* This is not AMDGPU driver, skip.*/
			drmFreeVersion(version);
			close(fd);
			continue;
		}

		drmFreeVersion(version);

		drm_amdgpu_fds[amd_index] = fd;
		amd_index++;
	}

	drmFreeDevices(devices, drm_count);
	return amd_index;
}
static bool
amdgpu_node_is_drm(int maj, int min)
{
	char path[64];
	struct stat sbuf;

	snprintf(path, sizeof(path), "/sys/dev/char/%d:%d/device/drm", maj, min);
	return stat(path, &sbuf) == 0;
}

static char *
amdgpu_get_device_from_fd(int fd)
{
	struct stat sbuf;
	char path[PATH_MAX + 1];
	unsigned int maj, min;

	if (fstat(fd, &sbuf))
		return NULL;

	maj = major(sbuf.st_rdev);
	min = minor(sbuf.st_rdev);

	if (!amdgpu_node_is_drm(maj, min) || !S_ISCHR(sbuf.st_mode))
		return NULL;

	snprintf(path, sizeof(path), "/sys/dev/char/%d:%d/device", maj, min);
	return strdup(path);
}

static int
amdgpu_hotunplug_trigger(const char *pathname)
{
	int len;
	int fd= -1;

	fd = open(pathname, O_WRONLY);
	if (fd <= 0 )
		goto release;

	len = write(fd, "1", 1);

	close(fd);

release:
	return len;
}

static bool
amdgpu_hotunplug_setup_test(bool render_mode, const struct amd_pci_unplug_setup *setup,
							struct amd_pci_unplug *unplug)
{
	char *tmp_str = NULL;
	bool ret = false;
	int r;
	uint32_t  major_version, minor_version;

	unplug->num_devices = amdgpu_open_devices(render_mode, MAX_CARDS_SUPPORTED,
											  unplug->drm_amdgpu_fds);
	if (unplug->num_devices == 0 )
		goto release;

	if (setup->open_device && setup->open_device2 && unplug->num_devices < 2) {
		/*Not enough board for the test*/
		printf("SKIP ... more than 1 GPU is required for this test\n");
		goto release;
	}

	tmp_str = amdgpu_get_device_from_fd(unplug->drm_amdgpu_fds[0]);
	abort_oom_if_null(tmp_str);
	unplug->sysfs_remove = realloc(tmp_str, strlen(tmp_str) * 2);
	abort_oom_if_null(unplug->sysfs_remove );
	strcat(unplug->sysfs_remove, "/remove");

	r = amdgpu_device_initialize(unplug->drm_amdgpu_fds[0], &major_version,
									 &minor_version, &unplug->device_handle);
	if (r != 0)
		goto release;

	if (minor_version < setup->minor_version_req)
		goto release;

	if (!setup->open_device) {
			/* device handle is not always required for test */
			/* but for drm version is required always */
		amdgpu_device_deinitialize(unplug->device_handle);
		unplug->device_handle = NULL;
	}
		/* TODO launch another process */
	if (setup->open_device2) {
		r = amdgpu_device_initialize(unplug->drm_amdgpu_fds[1], &major_version,
						   &minor_version, &unplug->device_handle2);
		if (r != 0)
			goto release;
		if (minor_version < setup->minor_version_req)
			goto release;
	}
	ret = true;
release:
	return ret;
}

static void
amdgpu_hotunplug_teardown_test(struct amd_pci_unplug *unplug)
{
	int i;
	if (unplug->device_handle) {
		amdgpu_device_deinitialize(unplug->device_handle);
		unplug->device_handle = NULL;
	}
	if (unplug->device_handle2) {
		amdgpu_device_deinitialize(unplug->device_handle2);
		unplug->device_handle2 = NULL;
	}
	for (i = 0; i < unplug->num_devices; i++) {
		if (unplug->drm_amdgpu_fds[i] >= 0 ) {
			close(unplug->drm_amdgpu_fds[i]);
			unplug->drm_amdgpu_fds[i] = -1;
		}
	}
	if (unplug->sysfs_remove) {
		free(unplug->sysfs_remove);
		unplug->sysfs_remove = NULL;
	}
}

static int
amdgpu_hotunplug_remove(struct amd_pci_unplug *unplug)
{
	int r = amdgpu_hotunplug_trigger(unplug->sysfs_remove);
	return r;
}

static int
amdgpu_hotunplug_rescan(void)
{
	int r = amdgpu_hotunplug_trigger("/sys/bus/pci/rescan");
	return r;
}

static int
amdgpu_cs_sync(amdgpu_context_handle context, unsigned int ip_type,	int ring,
				unsigned int seqno)
{
	struct amdgpu_cs_fence fence = {
		.context = context,
		.ip_type = ip_type,
		.ring = ring,
		.fence = seqno,
	};
	uint32_t expired;
	int ret;

	ret = amdgpu_cs_query_fence_status(&fence,
					   AMDGPU_TIMEOUT_INFINITE,
					   0, &expired);
	return ret;
}

static void *
amdgpu_nop_cs(void *handle)
{
	amdgpu_bo_handle ib_result_handle;
	void *ib_result_cpu;
	uint64_t ib_result_mc_address;
	int r;
	amdgpu_bo_list_handle bo_list;
	amdgpu_va_handle va_handle;
	amdgpu_context_handle context;
	struct amdgpu_cs_request ibs_request;
	struct amdgpu_cs_ib_info ib_info;
	int bo_cmd_size = 4096;
	struct amd_pci_unplug * unplug = handle;
	amdgpu_device_handle device_handle = unplug->device_handle;

	struct amdgpu_cmd_base * base_cmd = get_cmd_base();
	r = amdgpu_cs_ctx_create(device_handle, &context);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_alloc_and_map(device_handle, bo_cmd_size, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
				    &ib_result_handle, &ib_result_cpu,
				    &ib_result_mc_address, &va_handle);
	igt_assert_eq(r, 0);

	memset(ib_result_cpu, 0, bo_cmd_size);
	base_cmd->attach_buf(base_cmd, ib_result_cpu, bo_cmd_size);
	base_cmd->emit_repeat(base_cmd, GFX_COMPUTE_NOP , 16);

	r = amdgpu_bo_list_create(device_handle, 1, &ib_result_handle, NULL, &bo_list);
	igt_assert_eq(r, 0);

	memset(&ib_info, 0, sizeof(struct amdgpu_cs_ib_info));
	ib_info.ib_mc_address = ib_result_mc_address;
	ib_info.size = base_cmd->cdw;

	memset(&ibs_request, 0, sizeof(struct amdgpu_cs_request));
	ibs_request.ip_type = AMDGPU_HW_IP_GFX;
	ibs_request.ring = 0;
	ibs_request.number_of_ibs = 1;
	ibs_request.ibs = &ib_info;
	ibs_request.resources = bo_list;

	while (unplug->do_cs)
		amdgpu_cs_submit(context, 0, &ibs_request, 1);

	amdgpu_cs_sync(context, AMDGPU_HW_IP_GFX, 0, ibs_request.seq_no);
	amdgpu_bo_list_destroy(bo_list);
	amdgpu_bo_unmap_and_free(ib_result_handle, va_handle, ib_result_mc_address,
							4096);

	amdgpu_cs_ctx_free(context);
	free_cmd_base(base_cmd);

	return NULL;
}

static pthread_t*
amdgpu_create_cs_thread(struct amd_pci_unplug * unplug)
{
	int r;
	pthread_t *thread = malloc(sizeof(*thread));
	if (!thread)
		return NULL;

	unplug->do_cs = true;

	r = pthread_create(thread, NULL, &amdgpu_nop_cs, unplug);
	igt_assert_eq(r, 0);

	/* Give thread enough time to start*/
	usleep(1000000);
	return thread;
}

static void
amdgpu_wait_cs_thread(struct amd_pci_unplug * unplug, pthread_t *thread)
{
	unplug->do_cs = false;

	pthread_join(*thread, NULL);
	free(thread);
}

static void
amdgpu_hotunplug_test(bool render_mode, const struct amd_pci_unplug_setup *setup,
					  struct amd_pci_unplug * unplug,  bool with_cs)
{
	int r;
	pthread_t *thread = NULL;

	r = amdgpu_hotunplug_setup_test(render_mode, setup, unplug);
	igt_assert_eq(r , 1);

	if (with_cs)
		thread = amdgpu_create_cs_thread(unplug);

	r = amdgpu_hotunplug_remove(unplug);
	igt_assert_eq(r > 0, 1);

	if (with_cs)
		amdgpu_wait_cs_thread(unplug, thread);

	amdgpu_hotunplug_teardown_test(unplug);

	r = amdgpu_hotunplug_rescan();
	igt_assert_eq(r > 0, 1);
}

void
amdgpu_hotunplug_simple(struct amd_pci_unplug_setup *setup,
						struct amd_pci_unplug *unplug)
{
	memset(unplug, 0, sizeof(*unplug));
	amdgpu_hotunplug_test(true, setup, unplug, false);
}

 void
 amdgpu_hotunplug_with_cs(struct amd_pci_unplug_setup *setup,
 						 struct amd_pci_unplug *unplug)
{
	memset(unplug, 0, sizeof(*unplug));
	setup->open_device = true;
	amdgpu_hotunplug_test(true, setup, unplug, true);
}

void
amdgpu_hotunplug_with_exported_bo(struct amd_pci_unplug_setup *setup,
								  struct amd_pci_unplug *unplug)
{
	int r;
	uint32_t dma_buf_fd;
	unsigned int *ptr;
	amdgpu_bo_handle bo_handle;

	struct amdgpu_bo_alloc_request request = {
		.alloc_size = 4096,
		.phys_alignment = 4096,
		.preferred_heap = AMDGPU_GEM_DOMAIN_GTT,
		.flags = 0,
	};
	memset(unplug, 0, sizeof(*unplug));
	setup->open_device = true;

	r = amdgpu_hotunplug_setup_test(true, setup, unplug);
	igt_assert_eq(r , 1);

	r = amdgpu_bo_alloc(unplug->device_handle, &request, &bo_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_export(bo_handle, amdgpu_bo_handle_type_dma_buf_fd, &dma_buf_fd);
	igt_assert_eq(r, 0);

	ptr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, dma_buf_fd, 0);

	r = amdgpu_hotunplug_remove(unplug);
	igt_assert_eq(r > 0, 1);

	amdgpu_bo_free(bo_handle);

	amdgpu_hotunplug_teardown_test(unplug);

	*ptr = 0xdeafbeef;

	munmap(ptr, 4096);
	close (dma_buf_fd);

	r = amdgpu_hotunplug_rescan();
	igt_assert_eq(r > 0, 1);
}

void
amdgpu_hotunplug_with_exported_fence(struct amd_pci_unplug_setup *setup,
									 struct amd_pci_unplug *unplug)
{
	amdgpu_bo_handle ib_result_handle;
	void *ib_result_cpu;
	uint64_t ib_result_mc_address;
	uint32_t sync_obj_handle, sync_obj_handle2;
	int r;
	amdgpu_bo_list_handle bo_list;
	amdgpu_va_handle va_handle;
	amdgpu_context_handle context;
	struct amdgpu_cs_request ibs_request;
	struct amdgpu_cs_ib_info ib_info;
	struct amdgpu_cs_fence fence_status = {0};
	int shared_fd;
	int bo_cmd_size = 4096;
	struct amdgpu_cmd_base * base_cmd = get_cmd_base();

	memset(unplug, 0, sizeof(*unplug));
	setup->open_device = true;
	setup->open_device2 = true;


	r = amdgpu_hotunplug_setup_test(true, setup, unplug);
	if( r != 1) {
		goto release;
	}

	r = amdgpu_cs_ctx_create(unplug->device_handle, &context);
	igt_assert_eq(r, 0);

	r = amdgpu_bo_alloc_and_map(unplug->device_handle, bo_cmd_size, 4096,
				    AMDGPU_GEM_DOMAIN_GTT, 0,
				    &ib_result_handle, &ib_result_cpu,
				    &ib_result_mc_address, &va_handle);
	igt_assert_eq(r, 0);
	memset(ib_result_cpu, 0, bo_cmd_size);
	base_cmd->attach_buf(base_cmd, ib_result_cpu, bo_cmd_size);
	base_cmd->emit_repeat(base_cmd, GFX_COMPUTE_NOP , 16);

	r = amdgpu_bo_list_create(unplug->device_handle, 1, &ib_result_handle, NULL,
							  &bo_list);
	igt_assert_eq(r, 0);

	memset(&ib_info, 0, sizeof(struct amdgpu_cs_ib_info));
	ib_info.ib_mc_address = ib_result_mc_address;
	ib_info.size = base_cmd->cdw;

	memset(&ibs_request, 0, sizeof(struct amdgpu_cs_request));
	ibs_request.ip_type = AMDGPU_HW_IP_GFX;
	ibs_request.ring = 0;
	ibs_request.number_of_ibs = 1;
	ibs_request.ibs = &ib_info;
	ibs_request.resources = bo_list;

	r = amdgpu_cs_submit(context, 0, &ibs_request, 1);
	igt_assert_eq(r, 0);

	fence_status.context = context;
	fence_status.ip_type = AMDGPU_HW_IP_GFX;
	fence_status.ip_instance = 0;
	fence_status.fence = ibs_request.seq_no;

	amdgpu_cs_fence_to_handle(unplug->device_handle, &fence_status,
						AMDGPU_FENCE_TO_HANDLE_GET_SYNCOBJ, &sync_obj_handle);
	igt_assert_eq(r, 0);

	r = amdgpu_cs_export_syncobj(unplug->device_handle, sync_obj_handle, &shared_fd);
	igt_assert_eq(r, 0);

	r = amdgpu_cs_import_syncobj(unplug->device_handle2, shared_fd, &sync_obj_handle2);
	igt_assert_eq(r, 0);

	r = amdgpu_cs_destroy_syncobj(unplug->device_handle, sync_obj_handle);
	igt_assert_eq(r, 0);

	amdgpu_bo_list_destroy(bo_list);
	amdgpu_bo_unmap_and_free(ib_result_handle, va_handle, ib_result_mc_address,
							 4096);

	amdgpu_cs_ctx_free(context);

	r = amdgpu_hotunplug_remove(unplug);
	igt_assert_eq(r > 0, 1);

	r = amdgpu_cs_syncobj_wait(unplug->device_handle2, &sync_obj_handle2, 1, 100000000, 0, NULL);
	igt_assert_eq(r, 0);

	r = amdgpu_cs_destroy_syncobj(unplug->device_handle2, sync_obj_handle2);
	igt_assert_eq(r, 0);

	amdgpu_hotunplug_teardown_test(unplug);

	r = amdgpu_hotunplug_rescan();
	igt_assert_eq(r > 0, 1);
release:
	free_cmd_base(base_cmd);
}
