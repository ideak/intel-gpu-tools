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

#include "igt.h"
#include "igt_panfrost.h"
#include "igt_syncobj.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include "panfrost-job.h"
#include "panfrost_drm.h"

#define WIDTH          1920
#define HEIGHT         1080
#define CLEAR_COLOR    0xff7f7f7f

/* One tenth of a second */
#define SHORT_TIME_NSEC 100000000ull

/* Add the time that the bad job takes to timeout (sched->timeout) and the time that a reset can take */
#define BAD_JOB_TIME_NSEC (SHORT_TIME_NSEC + 500000000ull + 100000000ull)

#define NSECS_PER_SEC 1000000000ull

static uint64_t
abs_timeout(uint64_t duration)
{
        struct timespec current;
        clock_gettime(CLOCK_MONOTONIC, &current);
        return (uint64_t)current.tv_sec * NSECS_PER_SEC + current.tv_nsec + duration;
}

static void check_done(struct mali_job_descriptor_header *header)
{
        igt_assert(header->exception_status == 1 && header->fault_pointer == 0);
}

igt_main
{
        int fd;

        igt_fixture {
                fd = drm_open_driver(DRIVER_PANFROST);
        }

        igt_subtest("pan-submit") {
                struct panfrost_submit *submit;

                submit = igt_panfrost_null_job(fd);

                do_ioctl(fd, DRM_IOCTL_PANFROST_SUBMIT, submit->args);
                igt_assert(syncobj_wait(fd, &submit->args->out_sync, 1,
                                        abs_timeout(SHORT_TIME_NSEC), 0, NULL));
                check_done(submit->submit_bo->map);
                igt_panfrost_free_job(fd, submit);
        }

        igt_subtest("pan-submit-error-no-jc") {
                struct drm_panfrost_submit submit = {.jc = 0,};
                do_ioctl_err(fd, DRM_IOCTL_PANFROST_SUBMIT, &submit, EINVAL);
        }

        igt_subtest("pan-submit-error-bad-in-syncs") {
                struct panfrost_submit *submit;

                submit = igt_panfrost_null_job(fd);
                submit->args->in_syncs = 0ULL;
                submit->args->in_sync_count = 1;

                do_ioctl_err(fd, DRM_IOCTL_PANFROST_SUBMIT, submit->args, EFAULT);
                igt_panfrost_free_job(fd, submit);
        }

        igt_subtest("pan-submit-error-bad-bo-handles") {
                struct panfrost_submit *submit;

                submit = igt_panfrost_null_job(fd);
                submit->args->bo_handles = 0ULL;
                submit->args->bo_handle_count = 1;

                do_ioctl_err(fd, DRM_IOCTL_PANFROST_SUBMIT, submit->args, EFAULT);
                igt_panfrost_free_job(fd, submit);
        }

        igt_subtest("pan-submit-error-bad-requirements") {
                struct panfrost_submit *submit;

                submit = igt_panfrost_null_job(fd);
                submit->args->requirements = 2;

                do_ioctl_err(fd, DRM_IOCTL_PANFROST_SUBMIT, submit->args, EINVAL);
                igt_panfrost_free_job(fd, submit);
        }

        igt_subtest("pan-submit-error-bad-out-sync") {
                struct panfrost_submit *submit;

                submit = igt_panfrost_null_job(fd);
                submit->args->out_sync = -1;

                do_ioctl_err(fd, DRM_IOCTL_PANFROST_SUBMIT, submit->args, ENODEV);
                igt_panfrost_free_job(fd, submit);
        }

        igt_subtest("pan-reset") {
                int tmpfd = drm_open_driver(DRIVER_PANFROST);
                struct panfrost_submit *submit[2];
                struct mali_job_descriptor_header *headers[3];

                submit[0] = igt_panfrost_job_loop(fd);
                submit[1] = igt_panfrost_null_job(tmpfd);
                headers[0] = igt_panfrost_job_loop_get_job_header(submit[0], 0);
                headers[1] = igt_panfrost_job_loop_get_job_header(submit[0], 1);
                headers[2] = submit[1]->submit_bo->map;
                do_ioctl(fd, DRM_IOCTL_PANFROST_SUBMIT, submit[0]->args);
                do_ioctl(tmpfd, DRM_IOCTL_PANFROST_SUBMIT, submit[1]->args);
                /* First job should timeout, second job should complete right after the timeout */
                igt_assert(!syncobj_wait(fd, &submit[0]->args->out_sync, 1,
                                         abs_timeout(SHORT_TIME_NSEC), 0, NULL));
                igt_assert(syncobj_wait(fd, &submit[0]->args->out_sync, 1,
                                        abs_timeout(BAD_JOB_TIME_NSEC), 0, NULL));
                igt_assert(syncobj_wait(tmpfd, &submit[1]->args->out_sync, 1,
                                        abs_timeout(SHORT_TIME_NSEC), 0, NULL));

                /* At least one job header of the job loop should have its exception status set to 0 */
                igt_assert(headers[0]->exception_status != 1 || headers[1]->exception_status != 1);
                check_done(headers[2]);
                igt_panfrost_free_job(fd, submit[0]);
                igt_panfrost_free_job(tmpfd, submit[1]);
                close(tmpfd);
        }

        igt_subtest("pan-submit-and-close") {
                /* We need our own FD because we close it right after the job submission */
                int tmpfd = drm_open_driver(DRIVER_PANFROST);
                struct panfrost_submit *submit;

                submit = igt_panfrost_job_loop(tmpfd);
                do_ioctl(tmpfd, DRM_IOCTL_PANFROST_SUBMIT, submit->args);
                igt_panfrost_free_job(tmpfd, submit);
                close(tmpfd);
        }

        igt_subtest("pan-unhandled-pagefault") {
                struct mali_job_descriptor_header *header;
                struct panfrost_submit *submit;

                submit = igt_panfrost_write_value_job(fd, true);
                do_ioctl(fd, DRM_IOCTL_PANFROST_SUBMIT, submit->args);
                header = submit->submit_bo->map;
                igt_assert(syncobj_wait(fd, &submit->args->out_sync, 1,
                                        abs_timeout(SHORT_TIME_NSEC), 0, NULL));

                /* The job should get a JOB_BUS_FAULT, but it's not reflected
                 * in the job header because the MMU mapping is disabled (to kill
                 * the job immediately) before the job manager has a chance to
                 * update the exception status.
                 */
                igt_assert(header->exception_status != 1);
                igt_panfrost_free_job(fd, submit);

                /* Now make sure new jobs on this context get executed properly */
                submit = igt_panfrost_write_value_job(fd, false);
                do_ioctl(fd, DRM_IOCTL_PANFROST_SUBMIT, submit->args);
                header = submit->submit_bo->map;
                igt_assert(syncobj_wait(fd, &submit->args->out_sync, 1,
                                        abs_timeout(SHORT_TIME_NSEC), 0, NULL));
                check_done(header);
                igt_panfrost_free_job(fd, submit);
        }

        igt_fixture {
                close(fd);
        }
}
