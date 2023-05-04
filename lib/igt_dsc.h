/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef IGT_DSC_H
#define IGT_DSC_H

#include "igt_fb.h"
#include "igt_kms.h"

bool igt_is_dsc_supported(int drmfd, char *connector_name);
bool igt_is_fec_supported(int drmfd, char *connector_name);
bool igt_is_dsc_enabled(int drmfd, char *connector_name);
bool igt_is_force_dsc_enabled(int drmfd, char *connector_name);
int igt_force_dsc_enable(int drmfd, char *connector_name);
int igt_force_dsc_enable_bpc(int drmfd, char *connector_name, int bpc);
int igt_get_dsc_debugfs_fd(int drmfd, char *connector_name);
bool igt_is_dsc_output_format_supported_by_sink(int drmfd, char *connector_name,
						enum dsc_output_format output_format);
int igt_force_dsc_output_format(int drmfd, char *connector_name,
				enum dsc_output_format output_format);

#endif
