/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include "igt_dsc.h"
#include "igt_sysfs.h"

static bool check_dsc_debugfs(int drmfd, char *connector_name, const char *check_str)
{
	char file_name[128] = {0};
	char buf[512];

	sprintf(file_name, "%s/i915_dsc_fec_support", connector_name);

	igt_debugfs_read(drmfd, file_name, buf);

	return strstr(buf, check_str);
}

static int write_dsc_debugfs(int drmfd, char *connector_name, const char *file_name,
			     const char *write_buf)
{
	int debugfs_fd = igt_debugfs_dir(drmfd);
	int len = strlen(write_buf);
	int ret;
	char file_path[128] = {0};

	sprintf(file_path, "%s/%s", connector_name, file_name);

	ret = igt_sysfs_write(debugfs_fd, file_path, write_buf, len);

	close(debugfs_fd);

	if (ret > 0)
		return 0;

	return ret;
}

/*
 * igt_is_dsc_supported:
 * @drmfd: A drm file descriptor
 * @connector_name: Name of the libdrm connector we're going to use
 *
 * Returns: True if DSC is supported for the given connector, false otherwise.
 */
bool igt_is_dsc_supported(int drmfd, char *connector_name)
{
	return check_dsc_debugfs(drmfd, connector_name, "DSC_Sink_Support: yes");
}

/*
 * igt_is_fec_supported:
 * @drmfd: A drm file descriptor
 * @connector_name: Name of the libdrm connector we're going to use
 *
 * Returns: True if FEC is supported for the given connector, false otherwise.
 */
bool igt_is_fec_supported(int drmfd, char *connector_name)
{
	return check_dsc_debugfs(drmfd, connector_name, "FEC_Sink_Support: yes");
}

/*
 * igt_is_dsc_enabled:
 * @drmfd: A drm file descriptor
 * @connector_name: Name of the libdrm connector we're going to use
 *
 * Returns: True if DSC is enabled for the given connector, false otherwise.
 */
bool igt_is_dsc_enabled(int drmfd, char *connector_name)
{
	return check_dsc_debugfs(drmfd, connector_name, "DSC_Enabled: yes");
}

/*
 * igt_is_force_dsc_enabled:
 * @drmfd: A drm file descriptor
 * @connector_name: Name of the libdrm connector we're going to use
 *
 * Returns: True if DSC is force enabled (via debugfs) for the given connector,
 * false otherwise.
 */
bool igt_is_force_dsc_enabled(int drmfd, char *connector_name)
{
	return check_dsc_debugfs(drmfd, connector_name, "Force_DSC_Enable: yes");
}

/*
 * igt_force_dsc_enable:
 * @drmfd: A drm file descriptor
 * @connector_name: Name of the libdrm connector we're going to use
 *
 * Returns: 0 on success or negative error code, in case of failure.
 */
int igt_force_dsc_enable(int drmfd, char *connector_name)
{
	return write_dsc_debugfs(drmfd, connector_name, "i915_dsc_fec_support", "1");
}

/*
 * igt_force_dsc_enable_bpc:
 * @drmfd: A drm file descriptor
 * @connector_name: Name of the libdrm connector we're going to use
 * @bpc: Input BPC
 *
 * Returns: 0 on success or negative error code, in case of failure.
 */
int igt_force_dsc_enable_bpc(int drmfd, char *connector_name, int bpc)
{
	char buf[20] = {0};

	sprintf(buf, "%d", bpc);

	return write_dsc_debugfs(drmfd, connector_name, "i915_dsc_bpc", buf);
}

/*
 * igt_get_dsc_debugfs_fd:
 * @drmfd: A drm file descriptor
 * @connector_name: Name of the libdrm connector we're going to use
 *
 * Returns: fd of the DSC debugfs for the given connector, else returns -1.
 */
int igt_get_dsc_debugfs_fd(int drmfd, char *connector_name)
{
	char file_name[128] = {0};

	sprintf(file_name, "%s/i915_dsc_fec_support", connector_name);

	return openat(igt_debugfs_dir(drmfd), file_name, O_WRONLY);
}
