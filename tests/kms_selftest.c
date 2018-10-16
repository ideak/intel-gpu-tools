/* SPDX-License-Identifier: GPL-2.0 */
#include "igt.h"
#include "igt_kmod.h"

IGT_TEST_DESCRIPTION("Basic sanity check of KMS selftests.");

igt_main
{
	igt_kselftests("test-drm_modeset", NULL, NULL, NULL);
}
