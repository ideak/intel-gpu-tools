// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <limits.h>
#include <fcntl.h>
#include <string.h>

#include "igt.h"
#include "lib/igt_device.h"
#include "lib/igt_pm.h"
#include "lib/igt_syncobj.h"
#include "lib/intel_reg.h"

#include "xe_drm.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

#define MAX_N_ENGINES 16
#define NO_SUSPEND -1
#define NO_RPM -1

typedef struct {
	int fd_xe;
	struct pci_device *pci_xe;
	struct pci_device *pci_root;
} device_t;

/* runtime_usage is only available if kernel build CONFIG_PM_ADVANCED_DEBUG */
static bool runtime_usage_available(struct pci_device *pci)
{
	char name[PATH_MAX];
	snprintf(name, PATH_MAX, "/sys/bus/pci/devices/%04x:%02x:%02x.%01x/runtime_usage",
		 pci->domain, pci->bus, pci->dev, pci->func);
	return access(name, F_OK) == 0;
}

static int open_d3cold_allowed(struct pci_device *pci)
{
	char name[PATH_MAX];
	int fd;

	snprintf(name, PATH_MAX, "/sys/bus/pci/devices/%04x:%02x:%02x.%01x/d3cold_allowed",
		 pci->domain, pci->bus, pci->dev, pci->func);

	fd = open(name, O_RDWR);
	igt_assert_f(fd >= 0, "Can't open %s\n", name);

	return fd;
}

static void get_d3cold_allowed(struct pci_device *pci, char *d3cold_allowed)
{
	int fd = open_d3cold_allowed(pci);

	igt_assert(read(fd, d3cold_allowed, 2));
	close(fd);
}

static void set_d3cold_allowed(struct pci_device *pci,
			       const char *d3cold_allowed)
{
	int fd = open_d3cold_allowed(pci);

	igt_assert(write(fd, d3cold_allowed, 2));
	close(fd);
}

static bool setup_d3(device_t device, enum igt_acpi_d_state state)
{
	switch (state) {
	case IGT_ACPI_D3Cold:
		igt_require(igt_pm_acpi_d3cold_supported(device.pci_root));
		igt_pm_enable_pci_card_runtime_pm(device.pci_root, NULL);
		set_d3cold_allowed(device.pci_xe, "1\n");
		return true;
	case IGT_ACPI_D3Hot:
		set_d3cold_allowed(device.pci_xe, "0\n");
		return true;
	default:
		igt_debug("Invalid D3 Selection\n");
	}

	return false;
}

static bool in_d3(device_t device, enum igt_acpi_d_state state)
{
	uint16_t val;

	/* We need to wait for the autosuspend to kick in before we can check */
	if (!igt_wait_for_pm_status(IGT_RUNTIME_PM_STATUS_SUSPENDED))
		return false;

	if (runtime_usage_available(device.pci_xe) &&
	    igt_pm_get_runtime_usage(device.pci_xe) != 0)
		return false;

	switch (state) {
	case IGT_ACPI_D3Hot:
		igt_assert_eq(pci_device_cfg_read_u16(device.pci_xe,
						      &val, 0xd4), 0);
		return (val & 0x3) == 0x3;
	case IGT_ACPI_D3Cold:
		return igt_wait(igt_pm_get_acpi_real_d_state(device.pci_root) ==
				IGT_ACPI_D3Cold, 10000, 100);
	default:
		igt_info("Invalid D3 State\n");
		igt_assert(0);
	}

	return true;
}

static bool out_of_d3(device_t device, enum igt_acpi_d_state state)
{
	uint16_t val;

	/* Runtime resume needs to be immediate action without any wait */
	if (runtime_usage_available(device.pci_xe) &&
	    igt_pm_get_runtime_usage(device.pci_xe) <= 0)
		return false;

	if (igt_get_runtime_pm_status() != IGT_RUNTIME_PM_STATUS_ACTIVE)
		return false;

	switch (state) {
	case IGT_ACPI_D3Hot:
		igt_assert_eq(pci_device_cfg_read_u16(device.pci_xe,
						      &val, 0xd4), 0);
		return (val & 0x3) == 0;
	case IGT_ACPI_D3Cold:
		return igt_pm_get_acpi_real_d_state(device.pci_root) ==
			IGT_ACPI_D0;
	default:
		igt_info("Invalid D3 State\n");
		igt_assert(0);
	}

	return true;
}

static void
test_exec(device_t device, struct drm_xe_engine_class_instance *eci,
	  int n_engines, int n_execs, enum igt_suspend_state s_state,
	  enum igt_acpi_d_state d_state)
{
	uint32_t vm;
	uint64_t addr = 0x1a0000;
	struct drm_xe_sync sync[2] = {
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(&sync),
	};
	uint32_t engines[MAX_N_ENGINES];
	uint32_t bind_engines[MAX_N_ENGINES];
	uint32_t syncobjs[MAX_N_ENGINES];
	size_t bo_size;
	uint32_t bo = 0;
	struct {
		uint32_t batch[16];
		uint64_t pad;
		uint32_t data;
	} *data;
	int i, b, rpm_usage;
	bool check_rpm = (d_state == IGT_ACPI_D3Hot ||
			  d_state == IGT_ACPI_D3Cold);

	igt_assert(n_engines <= MAX_N_ENGINES);
	igt_assert(n_execs > 0);

	if (check_rpm)
		igt_assert(in_d3(device, d_state));

	vm = xe_vm_create(device.fd_xe, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);

	if (check_rpm)
		igt_assert(out_of_d3(device, d_state));

	bo_size = sizeof(*data) * n_execs;
	bo_size = ALIGN(bo_size + xe_cs_prefetch_size(device.fd_xe),
			xe_get_default_alignment(device.fd_xe));

	if (check_rpm && runtime_usage_available(device.pci_xe))
		rpm_usage = igt_pm_get_runtime_usage(device.pci_xe);

	bo = xe_bo_create(device.fd_xe, eci->gt_id, vm, bo_size);
	data = xe_bo_map(device.fd_xe, bo, bo_size);

	for (i = 0; i < n_engines; i++) {
		engines[i] = xe_engine_create(device.fd_xe, vm, eci, 0);
		bind_engines[i] = 0;
		syncobjs[i] = syncobj_create(device.fd_xe, 0);
	};

	sync[0].handle = syncobj_create(device.fd_xe, 0);

	xe_vm_bind_async(device.fd_xe, vm, bind_engines[0], bo, 0, addr,
			 bo_size, sync, 1);

	if (check_rpm && runtime_usage_available(device.pci_xe))
		igt_assert(igt_pm_get_runtime_usage(device.pci_xe) > rpm_usage);

	for (i = 0; i < n_execs; i++) {
		uint64_t batch_offset = (char *)&data[i].batch - (char *)data;
		uint64_t batch_addr = addr + batch_offset;
		uint64_t sdi_offset = (char *)&data[i].data - (char *)data;
		uint64_t sdi_addr = addr + sdi_offset;
		int e = i % n_engines;

		b = 0;
		data[i].batch[b++] = MI_STORE_DWORD_IMM_GEN4;
		data[i].batch[b++] = sdi_addr;
		data[i].batch[b++] = sdi_addr >> 32;
		data[i].batch[b++] = 0xc0ffee;
		data[i].batch[b++] = MI_BATCH_BUFFER_END;
		igt_assert(b <= ARRAY_SIZE(data[i].batch));

		sync[0].flags &= ~DRM_XE_SYNC_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_SIGNAL;
		sync[1].handle = syncobjs[e];

		exec.engine_id = engines[e];
		exec.address = batch_addr;

		if (e != i)
			syncobj_reset(device.fd_xe, &syncobjs[e], 1);

		xe_exec(device.fd_xe, &exec);

		igt_assert(syncobj_wait(device.fd_xe, &syncobjs[e], 1,
					INT64_MAX, 0, NULL));
		igt_assert_eq(data[i].data, 0xc0ffee);

		if (i == n_execs / 2 && s_state != NO_SUSPEND)
			igt_system_suspend_autoresume(s_state,
						      SUSPEND_TEST_NONE);
	}

	igt_assert(syncobj_wait(device.fd_xe, &sync[0].handle, 1, INT64_MAX, 0,
				NULL));

	if (check_rpm && runtime_usage_available(device.pci_xe))
		rpm_usage = igt_pm_get_runtime_usage(device.pci_xe);

	sync[0].flags |= DRM_XE_SYNC_SIGNAL;
	xe_vm_unbind_async(device.fd_xe, vm, bind_engines[0], 0, addr,
			   bo_size, sync, 1);
	igt_assert(syncobj_wait(device.fd_xe, &sync[0].handle, 1, INT64_MAX, 0,
NULL));

	for (i = 0; i < n_execs; i++)
		igt_assert_eq(data[i].data, 0xc0ffee);

	syncobj_destroy(device.fd_xe, sync[0].handle);
	for (i = 0; i < n_engines; i++) {
		syncobj_destroy(device.fd_xe, syncobjs[i]);
		xe_engine_destroy(device.fd_xe, engines[i]);
		if (bind_engines[i])
			xe_engine_destroy(device.fd_xe, bind_engines[i]);
	}

	munmap(data, bo_size);

	gem_close(device.fd_xe, bo);

	if (check_rpm && runtime_usage_available(device.pci_xe))
		igt_assert(igt_pm_get_runtime_usage(device.pci_xe) < rpm_usage);
	if (check_rpm)
		igt_assert(out_of_d3(device, d_state));

	xe_vm_destroy(device.fd_xe, vm);

	if (check_rpm)
		igt_assert(in_d3(device, d_state));
}

igt_main
{
	struct drm_xe_engine_class_instance *hwe;
	device_t device;
	char d3cold_allowed[2];
	const struct s_state {
		const char *name;
		enum igt_suspend_state state;
	} s_states[] = {
		{ "s2idle", SUSPEND_STATE_FREEZE },
		{ "s3", SUSPEND_STATE_S3 },
		{ "s4", SUSPEND_STATE_DISK },
		{ NULL },
	};
	const struct d_state {
		const char *name;
		enum igt_acpi_d_state state;
	} d_states[] = {
		{ "d3hot", IGT_ACPI_D3Hot },
		{ "d3cold", IGT_ACPI_D3Cold },
		{ NULL },
	};

	igt_fixture {
		memset(&device, 0, sizeof(device));
		device.fd_xe = drm_open_driver(DRIVER_XE);
		device.pci_xe = igt_device_get_pci_device(device.fd_xe);
		device.pci_root = igt_device_get_pci_root_port(device.fd_xe);

		xe_device_get(device.fd_xe);

		/* Always perform initial once-basic exec checking for health */
		for_each_hw_engine(device.fd_xe, hwe)
			test_exec(device, hwe, 1, 1, NO_SUSPEND, NO_RPM);

		get_d3cold_allowed(device.pci_xe, d3cold_allowed);
		igt_assert(igt_setup_runtime_pm(device.fd_xe));
	}

	for (const struct s_state *s = s_states; s->name; s++) {
		igt_subtest_f("%s-basic", s->name) {
			igt_system_suspend_autoresume(s->state,
						      SUSPEND_TEST_NONE);
		}

		igt_subtest_f("%s-basic-exec", s->name) {
			for_each_hw_engine(device.fd_xe, hwe)
				test_exec(device, hwe, 1, 2, s->state,
					  NO_RPM);
		}

		igt_subtest_f("%s-exec-after", s->name) {
			igt_system_suspend_autoresume(s->state,
						      SUSPEND_TEST_NONE);
			for_each_hw_engine(device.fd_xe, hwe)
				test_exec(device, hwe, 1, 2, NO_SUSPEND,
					  NO_RPM);
		}

		igt_subtest_f("%s-multiple-execs", s->name) {
			for_each_hw_engine(device.fd_xe, hwe)
				test_exec(device, hwe, 16, 32, s->state,
					  NO_RPM);
		}

		for (const struct d_state *d = d_states; d->name; d++) {
			igt_subtest_f("%s-%s-basic-exec", s->name, d->name) {
				igt_assert(setup_d3(device, d->state));
				for_each_hw_engine(device.fd_xe, hwe)
					test_exec(device, hwe, 1, 2, s->state,
						  NO_RPM);
			}
		}
	}

	for (const struct d_state *d = d_states; d->name; d++) {
		igt_subtest_f("%s-basic", d->name) {
			igt_assert(setup_d3(device, d->state));
			igt_assert(in_d3(device, d->state));
		}

		igt_subtest_f("%s-basic-exec", d->name) {
			igt_assert(setup_d3(device, d->state));
			for_each_hw_engine(device.fd_xe, hwe)
				test_exec(device, hwe, 1, 1,
					  NO_SUSPEND, d->state);
		}

		igt_subtest_f("%s-multiple-execs", d->name) {
			igt_assert(setup_d3(device, d->state));
			for_each_hw_engine(device.fd_xe, hwe)
				test_exec(device, hwe, 16, 32,
					  NO_SUSPEND, d->state);
		}
	}

	igt_fixture {
		set_d3cold_allowed(device.pci_xe, d3cold_allowed);
		igt_restore_runtime_pm();
		xe_device_put(device.fd_xe);
		close(device.fd_xe);
	}
}
