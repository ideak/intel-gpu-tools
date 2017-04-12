/*
 * Copyright © 2017 Red Hat
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
#include <unistd.h>
#include <sys/ioctl.h>
#include "drm.h"

IGT_TEST_DESCRIPTION("Basic check for drm sync objects.");

/* destroy a random handle */
static void
test_bad_destroy(int fd)
{
	struct drm_syncobj_destroy destroy;
	int ret;

	destroy.handle = 0xdeadbeef;
	destroy.pad = 0;

	ret = ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);

	igt_assert(ret == -1 && errno == EINVAL);
}

/* handle to fd a bad handle */
static void
test_bad_handle_to_fd(int fd)
{
	struct drm_syncobj_handle handle;
	int ret;

	handle.handle = 0xdeadbeef;
	handle.flags = 0;

	ret = ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle);

	igt_assert(ret == -1 && errno == EINVAL);
}

/* fd to handle a bad fd */
static void
test_bad_fd_to_handle(int fd)
{
	struct drm_syncobj_handle handle;
	int ret;

	handle.fd = -1;
	handle.flags = 0;

	ret = ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle);

	igt_assert(ret == -1 && errno == EINVAL);
}

/* fd to handle an fd but not a sync file one */
static void
test_illegal_fd_to_handle(int fd)
{
	struct drm_syncobj_handle handle;
	int ret;

	handle.fd = fd;
	handle.flags = 0;

	ret = ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle);

	igt_assert(ret == -1 && errno == EINVAL);
}

static void
test_bad_flags_fd_to_handle(int fd)
{
	struct drm_syncobj_handle handle = { 0 };
	int ret;

	handle.flags = 0xdeadbeef;
	ret = ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle);
	igt_assert(ret == -1 && errno == EINVAL);
}

static void
test_bad_flags_handle_to_fd(int fd)
{
	struct drm_syncobj_handle handle = { 0 };
	int ret;

	handle.flags = 0xdeadbeef;
	ret = ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle);
	igt_assert(ret == -1 && errno == EINVAL);
}

static void
test_bad_pad_handle_to_fd(int fd)
{
	struct drm_syncobj_handle handle = { 0 };
	int ret;

	handle.pad = 0xdeadbeef;
	ret = ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle);
	igt_assert(ret == -1 && errno == EINVAL);
}

static void
test_bad_pad_fd_to_handle(int fd)
{
	struct drm_syncobj_handle handle = { 0 };
	int ret;

	handle.pad = 0xdeadbeef;
	ret = ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle);
	igt_assert(ret == -1 && errno == EINVAL);
}



/* destroy with data in the padding */
static void
test_bad_destroy_pad(int fd)
{
	struct drm_syncobj_create create = { 0 };
	struct drm_syncobj_destroy destroy;
	int ret;

	ret = ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create);

	destroy.handle = create.handle;
	destroy.pad = 0xdeadbeef;

	ret = ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);

	igt_assert(ret == -1 && errno == EINVAL);

	destroy.handle = create.handle;
	destroy.pad = 0;

	ret = ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
	igt_assert(ret == 0);
}

static void
test_bad_create_flags(int fd)
{
	struct drm_syncobj_create create = { 0 };
	int ret;

	create.flags = 0xdeadbeef;
	ret = ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create);
	igt_assert(ret == -1 && errno == EINVAL);
}

/*
 * currently don't do handle deduplication
 * test we get a different handle back.
 */
static void
test_valid_cycle(int fd)
{
	int ret;
	struct drm_syncobj_create create = { 0 };
	struct drm_syncobj_handle handle = { 0 };
	struct drm_syncobj_destroy destroy = { 0 };
	uint32_t first_handle;

	ret = ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create);
	igt_assert(ret == 0);

	first_handle = create.handle;

	handle.handle = create.handle;
	ret = ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle);
	igt_assert(ret == 0);
	handle.handle = 0;
	ret = ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle);
	close(handle.fd);
	igt_assert(ret == 0);

	igt_assert(handle.handle != first_handle);

	destroy.handle = handle.handle;
	ret = ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
	igt_assert(ret == 0);

	destroy.handle = first_handle;
	ret = ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
	igt_assert(ret == 0);
}

static bool has_syncobj(int fd)
{
	uint64_t value;
	if (drmGetCap(fd, DRM_CAP_SYNCOBJ, &value))
		return false;
	return value ? true : false;
}

igt_main
{
	int fd;

	igt_fixture {
		fd = drm_open_driver(DRIVER_ANY);
		igt_require(has_syncobj(fd));
	}


	igt_subtest("bad-destroy")
		test_bad_destroy(fd);

	igt_subtest("bad-create-flags")
		test_bad_create_flags(fd);

	igt_subtest("bad-handle-to-fd")
		test_bad_handle_to_fd(fd);

	igt_subtest("bad-fd-to-handle")
		test_bad_fd_to_handle(fd);

	igt_subtest("bad-flags-handle-to-fd")
		test_bad_flags_handle_to_fd(fd);

	igt_subtest("bad-flags-fd-to-handle")
		test_bad_flags_fd_to_handle(fd);

	igt_subtest("bad-pad-handle-to-fd")
		test_bad_pad_handle_to_fd(fd);

	igt_subtest("bad-pad-fd-to-handle")
		test_bad_pad_fd_to_handle(fd);

	igt_subtest("illegal-fd-to-handle")
		test_illegal_fd_to_handle(fd);

	igt_subtest("bad-destroy-pad")
		test_bad_destroy_pad(fd);

	igt_subtest("test-valid-cycle")
		test_valid_cycle(fd);

}
