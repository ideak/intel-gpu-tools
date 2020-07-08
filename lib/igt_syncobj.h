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

#ifndef IGT_SYNCOBJ_H
#define IGT_SYNCOBJ_H

#include <stdint.h>
#include <stdbool.h>
#include <drm.h>

uint32_t syncobj_create(int fd, uint32_t flags);
void syncobj_destroy(int fd, uint32_t handle);
int __syncobj_handle_to_fd(int fd, struct drm_syncobj_handle *args);
int __syncobj_fd_to_handle(int fd, struct drm_syncobj_handle *args);
int syncobj_handle_to_fd(int fd, uint32_t handle, uint32_t flags);
uint32_t syncobj_fd_to_handle(int fd, int syncobj_fd, uint32_t flags);
void syncobj_import_sync_file(int fd, uint32_t handle, int sync_file);
int __syncobj_wait(int fd, struct drm_syncobj_wait *args);
int syncobj_wait_err(int fd, uint32_t *handles, uint32_t count,
		     uint64_t abs_timeout_nsec, uint32_t flags);
bool syncobj_wait(int fd, uint32_t *handles, uint32_t count,
		  uint64_t abs_timeout_nsec, uint32_t flags,
		  uint32_t *first_signaled);
int __syncobj_timeline_wait_ioctl(int fd,
				  struct drm_syncobj_timeline_wait *args);
bool syncobj_timeline_wait(int fd, uint32_t *handles, uint64_t *points,
			   unsigned num_handles,
			   int64_t timeout_nsec, unsigned flags,
			   uint32_t *first_signaled);
int syncobj_timeline_wait_err(int fd, uint32_t *handles, uint64_t *points,
			      unsigned num_handles,
			      int64_t timeout_nsec, unsigned flags);
void syncobj_reset(int fd, uint32_t *handles, uint32_t count);
void syncobj_signal(int fd, uint32_t *handles, uint32_t count);
void syncobj_timeline_query(int fd, uint32_t *handles, uint64_t *points,
			    uint32_t count);
void syncobj_binary_to_timeline(int fd, uint32_t timeline_handle,
				uint64_t point, uint32_t binary_handle);
void syncobj_timeline_to_binary(int fd, uint32_t binary_handle,
				uint32_t timeline_handle,
				uint64_t point,
				uint32_t flags);
void syncobj_timeline_to_timeline(int fd,
				  uint64_t timeline_dst, uint32_t point_dst,
				  uint64_t timeline_src, uint32_t point_src);
void syncobj_timeline_signal(int fd, uint32_t *handles, uint64_t *points,
			     uint32_t count);

#endif /* IGT_SYNCOBJ_H */
