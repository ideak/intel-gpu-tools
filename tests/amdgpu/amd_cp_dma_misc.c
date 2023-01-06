// SPDX-License-Identifier: MIT
// Copyright 2023 Advanced Micro Devices, Inc.

#include "igt.h"
#include "drmtest.h"
#include <amdgpu.h>
#include <amdgpu_drm.h>
#include "lib/amdgpu/amd_cp_dma.h"
#include "lib/amdgpu/amd_ip_blocks.h"

igt_main
{
	amdgpu_device_handle device;
	amdgpu_device_handle device2;
	uint32_t major, minor;
	int r;

	int drm_amdgpu_fds[MAX_CARDS_SUPPORTED];
	struct amdgpu_gpu_info gpu_info = {};
	struct amdgpu_gpu_info gpu_info2 = {};
	int num_devices = 0;

	const struct phase {
		const char *name;
		unsigned int src_memory;
		unsigned int dst_memory;
	} phase[] = {
		{ "GTT_to_VRAM",  AMDGPU_GEM_DOMAIN_GTT,  AMDGPU_GEM_DOMAIN_VRAM },
		{ "VRAM_to_GTT",  AMDGPU_GEM_DOMAIN_VRAM, AMDGPU_GEM_DOMAIN_GTT  },
		{ "VRAM_to_VRAM", AMDGPU_GEM_DOMAIN_VRAM, AMDGPU_GEM_DOMAIN_VRAM },
		{ },
	}, *p;

	const struct engine {
		const char *name;
		unsigned int ip_type;
	} engines[] = {
		{ "AMDGPU_HW_IP_GFX",		AMDGPU_HW_IP_GFX     },
		{ "AMDGPU_HW_IP_COMPUTE",   AMDGPU_HW_IP_COMPUTE },
		{ },
	}, *e;


	igt_fixture {
		num_devices = amdgpu_open_devices(true, MAX_CARDS_SUPPORTED, drm_amdgpu_fds);
		igt_require(num_devices > 0);
		r = amdgpu_device_initialize(drm_amdgpu_fds[0], &major,
										 &minor, &device);
		igt_require(r == 0);
		igt_info("Initialized amdgpu, driver version %d.%d\n", major, minor);
		r = amdgpu_query_gpu_info(device, &gpu_info);
		igt_assert_eq(r, 0);
		r = setup_amdgpu_ip_blocks(major, minor, &gpu_info, device);
		igt_assert_eq(r, 0);
		if (num_devices > 1) {
			/* do test for 2 only */
			igt_assert_eq(num_devices, 2);
			r = amdgpu_device_initialize(drm_amdgpu_fds[1],
					&major, &minor, &device2);
			igt_require(r == 0);
			igt_info("Initialized amdgpu, driver2 version %d.%d\n",
					major, minor);
			r = amdgpu_query_gpu_info(device2, &gpu_info2);
			igt_assert_eq(r, 0);
		}
	}
	if (amdgpu_cp_dma_misc_is_supported(&gpu_info)) {
		for (p = phase; p->name; p++) {
			for (e = engines; e->name; e++) {
				if (e->ip_type == AMDGPU_HW_IP_GFX &&
						asic_is_gfx_pipe_removed(&gpu_info))
					continue;
				igt_subtest_f("%s-%s0", p->name, e->name)
				amdgpu_cp_dma_generic(device, NULL, e->ip_type, p->src_memory,
						p->dst_memory);
			}
		}
	} else {
		igt_info("SKIP due to testing device has ASIC family %d that is not supported by CP-DMA test\n",
				gpu_info.family_id);
	}

	if (num_devices > 1 &&
			amdgpu_cp_dma_misc_p2p_is_supported(&gpu_info2)) {
		for (p = phase; p->name; p++) {
			for (e = engines; e->name; e++) {
				if (e->ip_type == AMDGPU_HW_IP_GFX &&
						asic_is_gfx_pipe_removed(&gpu_info2))
					continue;
				igt_subtest_f("%s-%s0", p->name, e->name)
				amdgpu_cp_dma_generic(device, device2, e->ip_type,
						p->src_memory, p->dst_memory);
			}
		}
	} else {
		igt_info("SKIP due to more than one ASIC is required or testing device has ASIC family %d that is not supported by CP-DMA P2P test\n",
				gpu_info2.family_id);
	}

	igt_fixture {
		amdgpu_device_deinitialize(device);
		close(drm_amdgpu_fds[0]);
		if (num_devices > 1) {
			amdgpu_device_deinitialize(device2);
			close(drm_amdgpu_fds[1]);
		}
	}
}
