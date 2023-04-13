// SPDX-License-Identifier: GPL-2.0 OR MIT
/**********************************************************
 * Copyright 2021-2022 VMware, Inc.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **********************************************************/

#include "igt_vmwgfx.h"

#include <stdatomic.h>

struct {
	bool stress_test;
} options;

static struct option long_options[] = {
	{ "stress-test", 0, 0, 's' },
	{ NULL, 0, 0, 0 },
};

IGT_TEST_DESCRIPTION("Test basic command buffer processing.");

static int parse_options(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 's':
		options.stress_test = 1;
		igt_info("stress-test mode\n");
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}
	return IGT_OPT_HANDLER_SUCCESS;
}

static void check_mob_create_map(int fd)
{
	struct vmw_mob *mob_obj;
	uint32 size = 4096;

	/* create a new mob */
	mob_obj = vmw_ioctl_mob_create(fd, size);
	/* takes the created mob and maps it */
	vmw_ioctl_mob_map(fd, mob_obj);
	/* test that mapping is successful */
	igt_assert_neq(mob_obj->map_count, 0);

	vmw_ioctl_mob_unmap(mob_obj);
	vmw_ioctl_mob_close_handle(fd, mob_obj);
}

static void check_buffer_create(int fd)
{
	struct vmw_surface *buffer_obj;
	struct vmw_mob *mob_obj;
	uint32 size = 4096;

	mob_obj = vmw_ioctl_mob_create(fd, size);
	/* creates a buffer from mob */
	buffer_obj = vmw_ioctl_buffer_create(fd, 0, size, mob_obj);
	/* checks that the buffer is allocated */
	igt_assert_eq(buffer_obj->base.backup_size, size);

	vmw_ioctl_surface_unref(fd, buffer_obj);
	vmw_ioctl_mob_close_handle(fd, mob_obj);
}

static void check_execbuf_submit_fence(int fd, int32 cid)
{
	struct vmw_execbuf *command_buffer;
	struct drm_vmw_fence_rep cmd_fence = { 0 };
	const uint32 buffer_size = 128;

	struct vmw_mob *mob;
	struct vmw_surface *src_buffer;
	struct vmw_surface *dst_buffer1, *dst_buffer2, *dst_buffer3;

	uint32_t total_size = 0;
	SVGA3dCmdDXBufferCopy copyCmd = { 0 };
	uint32_t total_cmd_len = sizeof(SVGA3dCmdHeader) + sizeof(copyCmd);

	SVGA3dCmdReadbackGBSurface cmd;
	struct vmw_mob fake_mob = { 0 };
	char *readback;

	char *data;
	int i;

	mob = vmw_ioctl_mob_create(fd, buffer_size);

	data = vmw_ioctl_mob_map(fd, mob);
	for (i = 0; i < buffer_size; ++i)
		data[i] = i;
	vmw_ioctl_mob_unmap(mob);

	src_buffer = vmw_ioctl_buffer_create(
		fd, SVGA3D_SURFACE_BIND_SHADER_RESOURCE, buffer_size, mob);
	dst_buffer1 = vmw_ioctl_buffer_create(
		fd, SVGA3D_SURFACE_BIND_SHADER_RESOURCE, buffer_size, NULL);
	dst_buffer2 = vmw_ioctl_buffer_create(
		fd, SVGA3D_SURFACE_BIND_SHADER_RESOURCE, buffer_size, NULL);
	dst_buffer3 = vmw_ioctl_buffer_create(
		fd, SVGA3D_SURFACE_BIND_SHADER_RESOURCE, buffer_size, NULL);
	/* Create command buffer */
	command_buffer = vmw_execbuf_create(fd, cid);
	igt_assert(command_buffer != NULL);

	copyCmd.src = src_buffer->base.handle;
	copyCmd.dest = dst_buffer1->base.handle;
	copyCmd.width = buffer_size;
	copyCmd.srcX = 0;
	copyCmd.destX = 0;
	vmw_execbuf_append(command_buffer, SVGA_3D_CMD_DX_BUFFER_COPY, &copyCmd,
			   sizeof(copyCmd), NULL, 0);
	total_size += total_cmd_len;
	igt_assert_eq(command_buffer->offset, total_size);
	igt_assert(command_buffer->offset < command_buffer->buffer_size);

	for (i = 0; i < 4096; ++i) {
		copyCmd.src = dst_buffer1->base.handle;
		copyCmd.dest = dst_buffer2->base.handle;
		vmw_execbuf_append(command_buffer, SVGA_3D_CMD_DX_BUFFER_COPY,
				   &copyCmd, sizeof(copyCmd), NULL, 0);
		total_size += total_cmd_len;
	}

	copyCmd.src = dst_buffer2->base.handle;
	copyCmd.dest = dst_buffer3->base.handle;
	vmw_execbuf_append(command_buffer, SVGA_3D_CMD_DX_BUFFER_COPY, &copyCmd,
			   sizeof(copyCmd), NULL, 0);
	total_size += total_cmd_len;

	igt_assert_eq(command_buffer->offset, total_size);
	igt_assert(command_buffer->offset < command_buffer->buffer_size);

	vmw_execbuf_submit(command_buffer, &cmd_fence);
	vmw_ioctl_fence_finish(fd, &cmd_fence);

	cmd.sid = dst_buffer1->base.handle;
	vmw_execbuf_append(command_buffer, SVGA_3D_CMD_READBACK_GB_SURFACE,
			   &cmd, sizeof(cmd), NULL, 0);
	vmw_execbuf_submit(command_buffer, &cmd_fence);
	vmw_ioctl_fence_finish(fd, &cmd_fence);

	fake_mob.size = dst_buffer1->base.buffer_size;
	fake_mob.handle = dst_buffer1->base.buffer_handle;
	fake_mob.map_handle = dst_buffer1->base.buffer_map_handle;
	readback = vmw_ioctl_mob_map(fd, &fake_mob);
	for (i = 0; i < buffer_size; i++) {
		int val = readback[i];

		igt_assert_eq(i, val);
	}
	vmw_ioctl_mob_unmap(&fake_mob);

	vmw_ioctl_surface_unref(fd, src_buffer);
	vmw_ioctl_surface_unref(fd, dst_buffer1);
	vmw_ioctl_surface_unref(fd, dst_buffer2);
	vmw_ioctl_surface_unref(fd, dst_buffer3);
	vmw_execbuf_destroy(command_buffer);
	vmw_ioctl_mob_close_handle(fd, mob);
}

static int32 vmw_ioctl_command2(int drm_fd, int32_t cid, void *commands,
				uint32_t size, struct drm_vmw_fence_rep *fence)
{
	struct drm_vmw_execbuf_arg arg = { 0 };
	int ret;
	const int argsize = sizeof(arg);

	memset(&arg, 0, sizeof(arg));

	arg.fence_rep = (unsigned long)fence;
	arg.commands = (unsigned long)commands;
	arg.command_size = size;
	arg.throttle_us = 0; /* deprecated */
	arg.version = DRM_VMW_EXECBUF_VERSION;
	arg.context_handle = cid;

	do {
		ret = drmCommandWrite(drm_fd, DRM_VMW_EXECBUF, &arg, argsize);
		if (ret == -EBUSY)
			usleep(1000);
	} while (ret == -ERESTART || ret == -EBUSY);
	if (ret)
		return 1;
	return 0;
}

static _Atomic(int32_t) context_id;
static const uint32_t max_tries = 100000;

static void *create_contexts(void *data)
{
	int fd = (uintptr_t)data;
	uint32_t i;

	for (i = 0; i < max_tries; ++i) {
		int32_t cid = vmw_ioctl_context_create(fd);

		atomic_store(&context_id, cid);

		vmw_ioctl_context_destroy(fd, cid);
	}
	return 0;
}

static void *submit_queries(void *data)
{
	int fd = (uintptr_t)data;
	uint32_t i;

	for (i = 0; i < max_tries; ++i) {
		struct vmw_execbuf *cmd_buf;
		SVGA3dCmdDXDefineQuery cmd = {
			.queryId = 0,
			.type = SVGA3D_QUERYTYPE_TIMESTAMP,
			.flags = 0,
		};
		int32_t cid = atomic_load(&context_id);

		cmd_buf = vmw_execbuf_create(fd, cid);

		vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DEFINE_QUERY, &cmd,
				   sizeof(cmd), NULL, 0);

		vmw_ioctl_command2(fd, cid, cmd_buf->buffer, cmd_buf->offset,
				   NULL);
		cmd_buf->offset = 0;
		vmw_execbuf_destroy(cmd_buf);
	}
	return 0;
}

static void execbuf_stress_test(int fd)
{
	pthread_t threads[2];
	void *status;
	int ret;

	ret = pthread_create(&threads[0], NULL, create_contexts,
			     (void *)(uintptr_t)fd);
	igt_assert_eq(ret, 0);
	ret = pthread_create(&threads[1], NULL, submit_queries,
			     (void *)(uintptr_t)fd);
	igt_assert_eq(ret, 0);

	pthread_join(threads[0], &status);
	pthread_join(threads[1], &status);
}

igt_main_args("st:", long_options, NULL, parse_options, NULL)
{
	int fd;
	int32 cid;

	igt_fixture
	{
		fd = drm_open_driver_render(DRIVER_VMWGFX);
		cid = vmw_ioctl_context_create(fd);
	}

	igt_describe("Test creation/mapping of a basic mob.");
	igt_subtest("mob-create-map")
	{
		check_mob_create_map(fd);
	}

	igt_describe("Test creation of a buffer surface from mob.");
	igt_subtest("buffer-create")
	{
		check_buffer_create(fd);
	}

	igt_describe("Test basic fencing on command buffers.");
	igt_subtest("execution-buffer-submit-sync")
	{
		check_execbuf_submit_fence(fd, cid);
	}

	if (options.stress_test) {
		igt_describe("Stress test synching cmd-buffers between threads.");
		igt_subtest("execution-buffer-stress-test")
		{
			execbuf_stress_test(fd);
		}
	}

	igt_fixture
	{
		vmw_ioctl_context_destroy(fd, cid);
		close(fd);
	}
}
