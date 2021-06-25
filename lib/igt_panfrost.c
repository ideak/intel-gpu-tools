/*
 * Copyright © 2016 Broadcom
 * Copyright © 2019 Collabora, Ltd.
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

#include <assert.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include "drmtest.h"
#include "igt_aux.h"
#include "igt_core.h"
#include "igt_panfrost.h"
#include "ioctl_wrappers.h"
#include "intel_reg.h"
#include "intel_chipset.h"
#include "panfrost_drm.h"
#include "panfrost-job.h"

/**
 * SECTION:igt_panfrost
 * @short_description: PANFROST support library
 * @title: PANFROST
 * @include: igt.h
 *
 * This library provides various auxiliary helper functions for writing PANFROST
 * tests.
 */

struct panfrost_bo *
igt_panfrost_gem_new(int fd, size_t size)
{
        struct panfrost_bo *bo = calloc(1, sizeof(*bo));

        struct drm_panfrost_create_bo create_bo = {
                .size = size,
        };

        do_ioctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &create_bo);

        bo->handle = create_bo.handle;
        bo->offset = create_bo.offset;
        bo->size = size;
        return bo;
}

void
igt_panfrost_free_bo(int fd, struct panfrost_bo *bo)
{
        if (!bo)
                return;

        if (bo->map)
                munmap(bo->map, bo->size);
        gem_close(fd, bo->handle);
        free(bo);
}

uint32_t
igt_panfrost_get_bo_offset(int fd, uint32_t handle)
{
        struct drm_panfrost_get_bo_offset get = {
                .handle = handle,
        };

        do_ioctl(fd, DRM_IOCTL_PANFROST_GET_BO_OFFSET, &get);

        return get.offset;
}

uint32_t
igt_panfrost_get_param(int fd, int param)
{
        struct drm_panfrost_get_param get = {
                .param = param,
        };

        do_ioctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &get);

        return get.value;
}

void *
igt_panfrost_mmap_bo(int fd, uint32_t handle, uint32_t size, unsigned prot)
{
        struct drm_panfrost_mmap_bo mmap_bo = {
                .handle = handle,
        };
        void *ptr;

        mmap_bo.handle = handle;
        do_ioctl(fd, DRM_IOCTL_PANFROST_MMAP_BO, &mmap_bo);

        ptr = mmap(0, size, prot, MAP_SHARED, fd, mmap_bo.offset);
        if (ptr == MAP_FAILED)
                return NULL;
        else
                return ptr;
}

void igt_panfrost_bo_mmap(int fd, struct panfrost_bo *bo)
{
        bo->map = igt_panfrost_mmap_bo(fd, bo->handle, bo->size,
                                  PROT_READ | PROT_WRITE);
        igt_assert(bo->map);
}

struct mali_job_descriptor_header *
igt_panfrost_job_loop_get_job_header(struct panfrost_submit *submit,
                                     unsigned job_idx)
{
        unsigned job_offset = ALIGN(sizeof(struct mali_job_descriptor_header) +
                                    sizeof(struct mali_payload_set_value),
                                    64) *
                              job_idx;

        igt_assert(job_idx <= 1);

        return submit->submit_bo->map + job_offset;
}

struct panfrost_submit *igt_panfrost_job_loop(int fd)
{
        /* We create 2 WRITE_VALUE jobs pointing to each other to form a loop.
         * Each WRITE_VALUE job resets the ->exception_status field of the
         * other job to allow re-execution (if we don't do that we end up with
         * an INVALID_DATA fault on the second execution).
         */
        struct panfrost_submit *submit;
        struct mali_job_descriptor_header header = {
                .job_type = JOB_TYPE_SET_VALUE,
                .job_barrier = 1,
                .unknown_flags = 5,
                .job_index = 1,
                .job_descriptor_size = 1,
        };

        /* .unknow = 3 means write 0 at the address specified in .out */
        struct mali_payload_set_value payload = {
                .unknown = 3,
        };
        uint32_t *bos;
        unsigned job1_offset = ALIGN(sizeof(header) + sizeof(payload), 64);
        unsigned job0_offset = 0;

        submit = malloc(sizeof(*submit));
	memset(submit, 0, sizeof(*submit));

        submit->submit_bo = igt_panfrost_gem_new(fd, ALIGN(sizeof(header) + sizeof(payload), 64) * 2);
        igt_panfrost_bo_mmap(fd, submit->submit_bo);

        /* Job 0 points to job 1 and has its WRITE_VALUE pointer pointing to
         * job 1 execption_status field.
         */
        header.next_job_64 = submit->submit_bo->offset + job1_offset;
        payload.out = submit->submit_bo->offset + job1_offset +
                      offsetof(struct mali_job_descriptor_header, exception_status);
        memcpy(submit->submit_bo->map + job0_offset, &header, sizeof(header));
        memcpy(submit->submit_bo->map + job0_offset + sizeof(header), &payload, sizeof(payload));

        /* Job 1 points to job 0 and has its WRITE_VALUE pointer pointing to
         * job 0 execption_status field.
         */
        header.next_job_64 = submit->submit_bo->offset + job0_offset;
        payload.out = submit->submit_bo->offset + job0_offset +
                      offsetof(struct mali_job_descriptor_header, exception_status);
        memcpy(submit->submit_bo->map + job1_offset, &header, sizeof(header));
        memcpy(submit->submit_bo->map + job1_offset + sizeof(header), &payload, sizeof(payload));

        submit->args = malloc(sizeof(*submit->args));
        memset(submit->args, 0, sizeof(*submit->args));
        submit->args->jc = submit->submit_bo->offset;

        bos = malloc(sizeof(*bos) * 1);
        bos[0] = submit->submit_bo->handle;

        submit->args->bo_handles = to_user_pointer(bos);
        submit->args->bo_handle_count = 1;

        igt_assert_eq(drmSyncobjCreate(fd, DRM_SYNCOBJ_CREATE_SIGNALED, &submit->args->out_sync), 0);

        return submit;
}

struct panfrost_submit *igt_panfrost_null_job(int fd)
{
        struct panfrost_submit *submit;
        struct mali_job_descriptor_header header = {
                .job_type = JOB_TYPE_NULL,
                .job_index = 1,
                .job_descriptor_size = 1,
        };
        uint32_t *bos;

        submit = malloc(sizeof(*submit));
	memset(submit, 0, sizeof(*submit));

        submit->submit_bo = igt_panfrost_gem_new(fd, sizeof(header));
        igt_panfrost_bo_mmap(fd, submit->submit_bo);

        memcpy(submit->submit_bo->map, &header, sizeof(header));

        submit->args = malloc(sizeof(*submit->args));
        memset(submit->args, 0, sizeof(*submit->args));
        submit->args->jc = submit->submit_bo->offset;

        bos = malloc(sizeof(*bos) * 1);
        bos[0] = submit->submit_bo->handle;

        submit->args->bo_handles = to_user_pointer(bos);
        submit->args->bo_handle_count = 1;

        igt_assert_eq(drmSyncobjCreate(fd, DRM_SYNCOBJ_CREATE_SIGNALED, &submit->args->out_sync), 0);

        return submit;
}

struct panfrost_submit *
igt_panfrost_write_value_job(int fd, bool trigger_page_fault)
{
        struct panfrost_submit *submit;
        struct mali_job_descriptor_header header = {
                .job_type = JOB_TYPE_SET_VALUE,
                .job_index = 1,
                .job_descriptor_size = 1,
        };

        /* .unknow = 3 means write 0 at the address specified in .out */
        struct mali_payload_set_value payload = {
                .unknown = 3,
        };
        uint32_t *bos;
        unsigned write_ptr_offset = sizeof(header) + sizeof(payload);

        submit = malloc(sizeof(*submit));
        memset(submit, 0, sizeof(*submit));

        submit->submit_bo = igt_panfrost_gem_new(fd, sizeof(header) + sizeof(payload) + sizeof(uint64_t));
        igt_panfrost_bo_mmap(fd, submit->submit_bo);

        payload.out = trigger_page_fault ?
                      0x0000deadbeef0000 :
                      submit->submit_bo->offset + write_ptr_offset;

        memcpy(submit->submit_bo->map, &header, sizeof(header));
        memcpy(submit->submit_bo->map + sizeof(header), &payload, sizeof(payload));
        memset(submit->submit_bo->map + write_ptr_offset, 0xff, sizeof(uint32_t));

        submit->args = malloc(sizeof(*submit->args));
        memset(submit->args, 0, sizeof(*submit->args));
        submit->args->jc = submit->submit_bo->offset;

        bos = malloc(sizeof(*bos) * 1);
        bos[0] = submit->submit_bo->handle;

        submit->args->bo_handles = to_user_pointer(bos);
        submit->args->bo_handle_count = 1;

        igt_assert_eq(drmSyncobjCreate(fd, DRM_SYNCOBJ_CREATE_SIGNALED, &submit->args->out_sync), 0);

        return submit;
}

void igt_panfrost_free_job(int fd, struct panfrost_submit *submit)
{
        free(from_user_pointer(submit->args->bo_handles));
        igt_panfrost_free_bo(fd, submit->submit_bo);
        igt_panfrost_free_bo(fd, submit->fb_bo);
        igt_panfrost_free_bo(fd, submit->scratchpad_bo);
        igt_panfrost_free_bo(fd, submit->tiler_scratch_bo);
        igt_panfrost_free_bo(fd, submit->tiler_heap_bo);
        igt_panfrost_free_bo(fd, submit->fbo);
        free(submit->args);
        free(submit);
}
