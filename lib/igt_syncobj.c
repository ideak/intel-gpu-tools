/*
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

#include <errno.h>
#include <xf86drm.h>

#include "igt.h"
#include "igt_syncobj.h"

/**
 * SECTION:igt_syncobj
 * @short_description: Library with syncobj helpers
 * @title: syncobj
 * @include: igt_syncobj.h
 *
 * This library contains helpers for sync object tests.
 */

static int
__syncobj_create(int fd, uint32_t *handle, uint32_t flags)
{
	struct drm_syncobj_create create = { 0 };
	int err = 0;

	create.flags = flags;
	if (igt_ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create)) {
		err = -errno;
		igt_assume(err);
		errno = 0;
	}
	*handle = create.handle;
	return err;
}

/**
 * syncobj_create
 * @fd: The DRM file descriptor
 * @flags: Flags to pass syncobj create
 *
 * Create a syncobj with the flags.
 *
 * Returns: A newly created syncobj
 */
uint32_t
syncobj_create(int fd, uint32_t flags)
{
	uint32_t handle;
	igt_assert_eq(__syncobj_create(fd, &handle, flags), 0);
	igt_assert(handle);
	return handle;
}

static int
__syncobj_destroy(int fd, uint32_t handle)
{
	struct drm_syncobj_destroy destroy = { 0 };
	int err = 0;

	destroy.handle = handle;
	if (igt_ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy)) {
		err = -errno;
		igt_assume(err);
		errno = 0;
	}
	return err;
}

/**
 * syncobj_destroy:
 * @fd: The DRM file descriptor
 * @handle: The handle to the syncobj to destroy
 * Destroy a syncobj.
 */
void
syncobj_destroy(int fd, uint32_t handle)
{
	igt_assert_eq(__syncobj_destroy(fd, handle), 0);
}

int
__syncobj_handle_to_fd(int fd, struct drm_syncobj_handle *args)
{
	int err = 0;
	if (igt_ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, args)) {
		err = -errno;
		igt_assume(err);
		errno = 0;
	}
	return err;
}

/**
 * syncobj_handle_to_fd:
 * @fd: The DRM file descriptor
 * @handle: Handle to syncobj
 * @flags: Flags to handle to fd ioctl.
 *
 * Convert a syncobj handle to an fd using the flags.
 *
 * Returns: a file descriptor (either syncobj or sync_file.
 */
int
syncobj_handle_to_fd(int fd, uint32_t handle, uint32_t flags)
{
	struct drm_syncobj_handle args = { 0 };
	args.handle = handle;
	args.flags = flags;
	igt_assert_eq(__syncobj_handle_to_fd(fd, &args), 0);
	igt_assert(args.fd >= 0);
	return args.fd;
}

int
__syncobj_fd_to_handle(int fd, struct drm_syncobj_handle *args)
{
	int err = 0;
	if (igt_ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, args)) {
		err = -errno;
		igt_assume(err);
		errno = 0;
	}
	return err;
}

/**
 * syncobj_fd_to_handle:
 * @fd: The DRM file descriptor
 * @syncobj_fd: syncobj fd to convert
 * @flags: Flags to the syncobj fd to handle ioctl.
 *
 * Convert a syncobj fd a syncobj handle using the flags.
 *
 * Returns: a syncobj handle.
 */
uint32_t
syncobj_fd_to_handle(int fd, int syncobj_fd, uint32_t flags)
{
	struct drm_syncobj_handle args = { 0 };
	args.fd = syncobj_fd;
	args.flags = flags;
	igt_assert_eq(__syncobj_fd_to_handle(fd, &args), 0);
	igt_assert(args.handle > 0);
	return args.handle;
}

/**
 * syncobj_import_sync_file:
 * @fd: The DRM file descriptor
 * @handle: Handle to the syncobt to import file into
 * @sync_file: The sync_file fd to import state from.
 *
 * Import a sync_file fd into a syncobj handle.
 */
void
syncobj_import_sync_file(int fd, uint32_t handle, int sync_file)
{
	struct drm_syncobj_handle args = { 0 };
	args.handle = handle;
	args.fd = sync_file;
	args.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
	igt_assert_eq(__syncobj_fd_to_handle(fd, &args), 0);
}

int
__syncobj_wait(int fd, struct drm_syncobj_wait *args)
{
	int err = 0;
	if (igt_ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, args)) {
		err = -errno;
		igt_assume(err);
		errno = 0;
	}
	return err;
}

int
syncobj_wait_err(int fd, uint32_t *handles, uint32_t count,
		 uint64_t abs_timeout_nsec, uint32_t flags)
{
	struct drm_syncobj_wait wait;

	wait.handles = to_user_pointer(handles);
	wait.timeout_nsec = abs_timeout_nsec;
	wait.count_handles = count;
	wait.flags = flags;
	wait.first_signaled = 0;
	wait.pad = 0;

	return __syncobj_wait(fd, &wait);
}

/**
 * syncobj_wait:
 * @fd: The DRM file descriptor
 * @handles: List of syncobj handles to wait for.
 * @count: Count of handles
 * @abs_timeout_nsec: Absolute wait timeout in nanoseconds.
 * @flags: Wait ioctl flags.
 * @first_signaled: Returned handle for first signaled syncobj.
 *
 * Waits in the kernel for any/all the requested syncobjs
 * using the timeout and flags.
 * Returns: bool value - false = timedout, true = signaled
 */
bool
syncobj_wait(int fd, uint32_t *handles, uint32_t count,
	     uint64_t abs_timeout_nsec, uint32_t flags,
	     uint32_t *first_signaled)
{
	struct drm_syncobj_wait wait;
	int ret;

	wait.handles = to_user_pointer(handles);
	wait.timeout_nsec = abs_timeout_nsec;
	wait.count_handles = count;
	wait.flags = flags;
	wait.first_signaled = 0;
	wait.pad = 0;

	ret = __syncobj_wait(fd, &wait);
	if (ret == -ETIME)
		return false;

	igt_assert_eq(ret, 0);
	if (first_signaled)
		*first_signaled = wait.first_signaled;

	return true;
}

static int
__syncobj_reset(int fd, uint32_t *handles, uint32_t count)
{
	struct drm_syncobj_array array = { 0 };
	int err = 0;

	array.handles = to_user_pointer(handles);
	array.count_handles = count;
	if (igt_ioctl(fd, DRM_IOCTL_SYNCOBJ_RESET, &array)) {
		err = -errno;
		igt_assume(err);
		errno = 0;
	}
	return err;
}

/**
 * syncobj_reset:
 * @fd: The DRM file descriptor.
 * @handles: Array of syncobj handles to reset
 * @count: Count of syncobj handles.
 *
 * Reset state of a set of syncobjs.
 */
void
syncobj_reset(int fd, uint32_t *handles, uint32_t count)
{
	igt_assert_eq(__syncobj_reset(fd, handles, count), 0);
}

static int
__syncobj_signal(int fd, uint32_t *handles, uint32_t count)
{
	struct drm_syncobj_array array = { 0 };
	int err = 0;

	array.handles = to_user_pointer(handles);
	array.count_handles = count;
	if (igt_ioctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &array)) {
		err = -errno;
		igt_assume(err);
		errno = 0;
	}
	return err;
}

/**
 * syncobj_signal:
 * @fd: The DRM file descriptor.
 * @handles: Array of syncobj handles to signal
 * @count: Count of syncobj handles.
 *
 * Signal a set of syncobjs.
 */
void
syncobj_signal(int fd, uint32_t *handles, uint32_t count)
{
	igt_assert_eq(__syncobj_signal(fd, handles, count), 0);
}

static int
__syncobj_timeline_signal(int fd, uint32_t *handles, uint64_t *points, uint32_t count)
{
	struct drm_syncobj_timeline_array array = { 0 };
	int err = 0;

	array.handles = to_user_pointer(handles);
	array.points = to_user_pointer(points);
	array.count_handles = count;
	if (igt_ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &array)) {
		err = -errno;
		igt_assume(err);
		errno = 0;
	}
	return err;
}

/**
 * syncobj_signal:
 * @fd: The DRM file descriptor.
 * @handles: Array of syncobj handles to signal
 * @points: List of point of handles to signal.
 * @count: Count of syncobj handles.
 *
 * Signal a set of syncobjs.
 */
void
syncobj_timeline_signal(int fd, uint32_t *handles, uint64_t *points, uint32_t count)
{
	igt_assert_eq(__syncobj_timeline_signal(fd, handles, points, count), 0);
}
int
__syncobj_timeline_wait_ioctl(int fd, struct drm_syncobj_timeline_wait *args)
{
	int err = 0;
	if (igt_ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, args)) {
		err = -errno;
		igt_assume(err);
		errno = 0;
	}
	return err;
}
static int
__syncobj_timeline_wait(int fd, uint32_t *handles, uint64_t *points,
			unsigned num_handles,
			int64_t timeout_nsec, unsigned flags,
			uint32_t *first_signaled)
{
	struct drm_syncobj_timeline_wait args;
	int ret;

	args.handles = to_user_pointer(handles);
	args.points = to_user_pointer(points);
	args.timeout_nsec = timeout_nsec;
	args.count_handles = num_handles;
	args.flags = flags;
	args.first_signaled = 0;
	args.pad = 0;

	ret = igt_ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &args);
	if (ret < 0) {
		ret = -errno;
		igt_assume(ret);
		errno = 0;
	}

	if (first_signaled)
		*first_signaled = args.first_signaled;

	return ret;
}
int
syncobj_timeline_wait_err(int fd, uint32_t *handles, uint64_t *points,
			  unsigned num_handles,
			  int64_t timeout_nsec, unsigned flags)
{
	return __syncobj_timeline_wait(fd, handles, points, num_handles,
				       timeout_nsec, flags, NULL);
}

/**
 * syncobj_timeline_wait:
 * @fd: The DRM file descriptor
 * @handles: List of syncobj handles to wait for.
 * @points: List of point of handles to wait for.
 * @num_handles: Count of handles
 * @timeout_nsec: Absolute wait timeout in nanoseconds.
 * @flags: Wait ioctl flags.
 * @first_signaled: Returned handle for first signaled syncobj.
 *
 * Waits in the kernel for any/all the requested syncobjs timeline point
 * using the timeout and flags.
 * Returns: bool value - false = timedout, true = signaled
 */
bool
syncobj_timeline_wait(int fd, uint32_t *handles, uint64_t *points,
		      unsigned num_handles,
		      int64_t timeout_nsec, unsigned flags,
		      uint32_t *first_signaled)
{
	int ret;

	ret = __syncobj_timeline_wait(fd, handles, points, num_handles,
				      timeout_nsec, flags, first_signaled);
	if (ret == -ETIME)
		return false;
	igt_assert_eq(ret, 0);

	return true;

}

static int
__syncobj_timeline_query(int fd, uint32_t *handles, uint64_t *points,
			 uint32_t handle_count)
{
	struct drm_syncobj_timeline_array args;
	int ret;

	args.handles = to_user_pointer(handles);
	args.points = to_user_pointer(points);
	args.count_handles = handle_count;
	args.flags = 0;

	ret = igt_ioctl(fd, DRM_IOCTL_SYNCOBJ_QUERY, &args);
	if (ret) {
		ret = -errno;
		igt_assume(ret);
		errno = 0;
	}

	return ret;
}

/**
 * syncobj_timeline_query:
 * @fd: The DRM file descriptor.
 * @handles: Array of syncobj handles.
 * @points: Array of syncobj points queried.
 * @count: Count of syncobj handles.
 *
 * Queries a set of syncobjs.
 */
void
syncobj_timeline_query(int fd, uint32_t *handles, uint64_t *points,
		       uint32_t count)
{
	igt_assert_eq(__syncobj_timeline_query(fd, handles, points, count), 0);
}

static int
__syncobj_transfer(int fd,
		   uint32_t handle_dst, uint64_t point_dst,
		   uint32_t handle_src, uint64_t point_src,
		   uint32_t flags)
{
	struct drm_syncobj_transfer args;
	int ret;

	args.src_handle = handle_src;
	args.dst_handle = handle_dst;
	args.src_point = point_src;
	args.dst_point = point_dst;
	args.flags = flags;
	args.pad = 0;
	ret = igt_ioctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &args);
	if (ret) {
		ret = -errno;
		igt_assume(ret);
		errno = 0;
	}

	return ret;
}

/**
 * syncobj_binary_to_timeline:
 * @fd: The DRM file descriptor.
 * @timeline_handle: A syncobj timeline handle
 * @point: A syncobj timeline point in the timeline handle
 * @binary_handle: A syncobj binary handle
 *
 * Transfers a DMA fence from a binary syncobj into a timeline syncobj
 * at a given point on the timeline.
 */
void
syncobj_binary_to_timeline(int fd, uint32_t timeline_handle,
			   uint64_t point, uint32_t binary_handle)
{
	igt_assert_eq(__syncobj_transfer(fd,
					 timeline_handle, point,
					 binary_handle, 0, 0), 0);
}

/**
 * syncobj_binary_to_timeline:
 * @fd: The DRM file descriptor.
 * @binary_handle: A syncobj binary handle
 * @timeline_handle: A syncobj timeline handle
 * @point: A syncobj timeline point in the timeline handle
 *
 * Transfers DMA fence from a given point from timeline syncobj into a
 * binary syncobj.
 */
void
syncobj_timeline_to_binary(int fd, uint32_t binary_handle,
			   uint32_t timeline_handle,
			   uint64_t point,
			   uint32_t flags)
{
	igt_assert_eq(__syncobj_transfer(fd,
					 binary_handle, 0,
					 timeline_handle, point,
					 flags), 0);
}

/**
 * syncobj_timeline_to_timeline:
 * @fd: The DRM file descriptor.
 * @timeline_src: A timeline syncobj handle
 * @timeline_dst: A timeline syncobj handle
 * @point_src: A point on the source timeline syncobj
 * @point_dst: A point on the destination timeline syncobj
 *
 * query a set of syncobjs.
 */
void
syncobj_timeline_to_timeline(int fd,
			     uint64_t timeline_dst, uint32_t point_dst,
			     uint64_t timeline_src, uint32_t point_src)
{
	igt_assert_eq(__syncobj_transfer(fd,
					 timeline_dst, point_dst,
					 timeline_src, point_src, 0), 0);
}
