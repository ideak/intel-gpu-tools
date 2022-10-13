/* SPDX-License-Identifier: MIT
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "igt.h"
#include "drmtest.h"

#include <amdgpu.h>
#include <amdgpu_drm.h>
#include "lib/amdgpu/amd_PM4.h"
#include "lib/amdgpu/amd_pci_unplug.h"
#include "lib/amdgpu/amd_ip_blocks.h"


igt_main
{

	struct amd_pci_unplug_setup setup = {0};
	struct amd_pci_unplug unplug = {0};

	igt_fixture {
		setup.minor_version_req = 46;
	}

	igt_subtest("amdgpu_hotunplug_simple")
		amdgpu_hotunplug_simple(&setup, &unplug);

	igt_subtest("amdgpu_hotunplug_with_cs")
		amdgpu_hotunplug_with_cs(&setup, &unplug);

		/*TODO about second GPU*/
	igt_subtest("amdgpu_hotunplug_with_exported_bo")
		amdgpu_hotunplug_with_exported_bo(&setup, &unplug);

	igt_subtest("amdgpu_hotunplug_with_exported_fence")
		amdgpu_hotunplug_with_exported_fence(&setup, &unplug);

	igt_fixture { }
}
