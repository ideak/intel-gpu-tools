/*
 * Copyright © 2017 Keith Packard
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

/** @file kms_lease.c
 *
 * This is a test of DRM leases
 */


#include "igt.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <drm.h>

IGT_TEST_DESCRIPTION("Test of CreateLease.");

struct local_drm_mode_create_lease {
        /** Pointer to array of object ids (__u32) */
        __u64 object_ids;
        /** Number of object ids */
        __u32 object_count;
        /** flags for new FD (O_CLOEXEC, etc) */
        __u32 flags;

        /** Return: unique identifier for lessee. */
        __u32 lessee_id;
        /** Return: file descriptor to new drm_master file */
        __u32 fd;
};

struct local_drm_mode_list_lessees {
        /** Number of lessees.
         * On input, provides length of the array.
         * On output, provides total number. No
         * more than the input number will be written
         * back, so two calls can be used to get
         * the size and then the data.
         */
        __u32 count_lessees;
        __u32 pad;

        /** Pointer to lessees.
         * pointer to __u64 array of lessee ids
         */
        __u64 lessees_ptr;
};

struct local_drm_mode_get_lease {
        /** Number of leased objects.
         * On input, provides length of the array.
         * On output, provides total number. No
         * more than the input number will be written
         * back, so two calls can be used to get
         * the size and then the data.
         */
        __u32 count_objects;
        __u32 pad;

        /** Pointer to objects.
         * pointer to __u32 array of object ids
         */
        __u64 objects_ptr;
};

/**
 * Revoke lease
 */
struct local_drm_mode_revoke_lease {
        /** Unique ID of lessee
         */
        __u32 lessee_id;
};


#define LOCAL_DRM_IOCTL_MODE_CREATE_LEASE     DRM_IOWR(0xC6, struct local_drm_mode_create_lease)
#define LOCAL_DRM_IOCTL_MODE_LIST_LESSEES     DRM_IOWR(0xC7, struct local_drm_mode_list_lessees)
#define LOCAL_DRM_IOCTL_MODE_GET_LEASE        DRM_IOWR(0xC8, struct local_drm_mode_get_lease)
#define LOCAL_DRM_IOCTL_MODE_REVOKE_LEASE     DRM_IOWR(0xC9, struct local_drm_mode_revoke_lease)

typedef struct {
	int fd;
	uint32_t lessee_id;
	igt_display_t display;
	struct igt_fb primary_fb;
	igt_output_t *output;
	drmModeModeInfo *mode;
} lease_t;

typedef struct {
	lease_t master;
	enum pipe pipe;
	uint32_t crtc_id;
	uint32_t connector_id;
	uint32_t plane_id;
} data_t;

static uint32_t pipe_to_crtc_id(igt_display_t *display, enum pipe pipe)
{
	return display->pipes[pipe].crtc_id;
}

static enum pipe crtc_id_to_pipe(igt_display_t *display, uint32_t crtc_id)
{
	enum pipe pipe;

	for (pipe = 0; pipe < display->n_pipes; pipe++)
		if (display->pipes[pipe].crtc_id == crtc_id)
			return pipe;
	return -1;
}

static igt_output_t *connector_id_to_output(igt_display_t *display, uint32_t connector_id)
{
	drmModeConnector		connector;

	connector.connector_id = connector_id;
	return igt_output_from_connector(display, &connector);
}

static int prepare_crtc(lease_t *lease, uint32_t connector_id, uint32_t crtc_id)
{
	drmModeModeInfo *mode;
	igt_display_t *display = &lease->display;
	igt_output_t *output = connector_id_to_output(display, connector_id);
	enum pipe pipe = crtc_id_to_pipe(display, crtc_id);
	igt_plane_t *primary;
	int ret;

	if (!output)
		return -ENOENT;

	/* select the pipe we want to use */
	igt_output_set_pipe(output, pipe);

	/* create and set the primary plane fb */
	mode = igt_output_get_mode(output);
	igt_create_color_fb(lease->fd, mode->hdisplay, mode->vdisplay,
			    DRM_FORMAT_XRGB8888,
			    LOCAL_DRM_FORMAT_MOD_NONE,
			    0.0, 0.0, 0.0,
			    &lease->primary_fb);

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, &lease->primary_fb);

	ret = igt_display_try_commit2(display, COMMIT_LEGACY);

	if (ret)
		return ret;

	igt_wait_for_vblank(lease->fd, pipe);

	lease->output = output;
	lease->mode = mode;
	return 0;
}

static void cleanup_crtc(lease_t *lease, igt_output_t *output)
{
	igt_display_t *display = &lease->display;
	igt_plane_t *primary;

	igt_remove_fb(lease->fd, &lease->primary_fb);

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, NULL);

	igt_output_set_pipe(output, PIPE_ANY);
	igt_display_commit(display);
}

static int create_lease(int fd, struct local_drm_mode_create_lease *mcl)
{
	int err = 0;

	if (igt_ioctl(fd, LOCAL_DRM_IOCTL_MODE_CREATE_LEASE, mcl))
		err = -errno;
	return err;
}

static int revoke_lease(int fd, struct local_drm_mode_revoke_lease *mrl)
{
	int err = 0;

	if (igt_ioctl(fd, LOCAL_DRM_IOCTL_MODE_REVOKE_LEASE, mrl))
		err = -errno;
	return err;
}

static int list_lessees(int fd, struct local_drm_mode_list_lessees *mll)
{
	int err = 0;

	if (igt_ioctl(fd, LOCAL_DRM_IOCTL_MODE_LIST_LESSEES, mll))
		err = -errno;
	return err;
}

static int get_lease(int fd, struct local_drm_mode_get_lease *mgl)
{
	int err = 0;

	if (igt_ioctl(fd, LOCAL_DRM_IOCTL_MODE_GET_LEASE, mgl))
		err = -errno;
	return err;
}

static int make_lease(data_t *data, lease_t *lease)
{
	uint32_t object_ids[3];
	struct local_drm_mode_create_lease mcl;
	int ret;

	mcl.object_ids = (uint64_t) (uintptr_t) &object_ids[0];
	mcl.object_count = 0;
	mcl.flags = 0;

	object_ids[mcl.object_count++] = data->connector_id;
	object_ids[mcl.object_count++] = data->crtc_id;
	/* We use universal planes, must add the primary plane */
	object_ids[mcl.object_count++] = data->plane_id;

	ret = create_lease(data->master.fd, &mcl);

	if (ret)
		return ret;

	lease->fd = mcl.fd;
	lease->lessee_id = mcl.lessee_id;
	return 0;
}

static void terminate_lease(lease_t *lease)
{
	close(lease->fd);
}

static int paint_fb(int drm_fd, struct igt_fb *fb, const char *test_name,
		    const char *mode_format_str, const char *connector_str, const char *pipe_str)
{
	cairo_t *cr;

	cr = igt_get_cairo_ctx(drm_fd, fb);

	igt_paint_color_gradient(cr, 0, 0, fb->width, fb->height, 1, 1, 1);
	igt_paint_test_pattern(cr, fb->width, fb->height);

	cairo_move_to(cr, fb->width / 2, fb->height / 2);
	cairo_set_font_size(cr, 36);
	igt_cairo_printf_line(cr, align_hcenter, 10, "%s", test_name);
	igt_cairo_printf_line(cr, align_hcenter, 10, "%s", mode_format_str);
	igt_cairo_printf_line(cr, align_hcenter, 10, "%s", connector_str);
	igt_cairo_printf_line(cr, align_hcenter, 10, "%s", pipe_str);

	cairo_destroy(cr);

	return 0;
}

static void simple_lease(data_t *data)
{
	lease_t lease;

	/* Create a valid lease */
	igt_assert_eq(make_lease(data, &lease), 0);

	igt_display_init(&lease.display, lease.fd);

	/* Set a mode on the leased output */
	igt_assert_eq(0, prepare_crtc(&lease, data->connector_id, data->crtc_id));

	/* Paint something attractive */
	paint_fb(lease.fd, &lease.primary_fb, "simple_lease",
		 lease.mode->name, igt_output_name(lease.output), kmstest_pipe_name(data->pipe));
	igt_debug_wait_for_keypress("lease");
	cleanup_crtc(&lease,
		     connector_id_to_output(&lease.display, data->connector_id));

	terminate_lease(&lease);
}

/* Test listing lessees */
static void lessee_list(data_t *data)
{
	lease_t lease;
	struct local_drm_mode_list_lessees mll;
	uint32_t lessees[1];

	mll.pad = 0;

	/* Create a valid lease */
	igt_assert_eq(make_lease(data, &lease), 0);

	/* check for nested leases */
	mll.count_lessees = 0;
	mll.lessees_ptr = 0;
	igt_assert_eq(list_lessees(lease.fd, &mll), 0);
	igt_assert_eq(mll.count_lessees, 0);

	/* Get the number of lessees */
	mll.count_lessees = 0;
	mll.lessees_ptr = 0;
	igt_assert_eq(list_lessees(data->master.fd, &mll), 0);

	/* Make sure there's a single lessee */
	igt_assert_eq(mll.count_lessees, 1);

	/* invalid ptr */
	igt_assert_eq(list_lessees(data->master.fd, &mll), -EFAULT);

	mll.lessees_ptr = (uint64_t) (uintptr_t) &lessees[0];

	igt_assert_eq(list_lessees(data->master.fd, &mll), 0);

	/* Make sure there's a single lessee */
	igt_assert_eq(mll.count_lessees, 1);

	/* Make sure the listed lease is the same as the one we created */
	igt_assert_eq(lessees[0], lease.lessee_id);

	/* invalid pad */
	mll.pad = -1;
	igt_assert_eq(list_lessees(data->master.fd, &mll), -EINVAL);
	mll.pad = 0;

	terminate_lease(&lease);

	/* Make sure the lease is gone */
	igt_assert_eq(list_lessees(data->master.fd, &mll), 0);
	igt_assert_eq(mll.count_lessees, 0);
}

/* Test getting the contents of a lease */
static void lease_get(data_t *data)
{
	lease_t lease;
	struct local_drm_mode_get_lease mgl;
	int num_leased_obj = 3;
	uint32_t objects[num_leased_obj];
	int o;

	mgl.pad = 0;

	/* Create a valid lease */
	igt_assert_eq(make_lease(data, &lease), 0);

	/* Get the number of objects */
	mgl.count_objects = 0;
	mgl.objects_ptr = 0;
	igt_assert_eq(get_lease(lease.fd, &mgl), 0);

	/* Make sure it's 2 */
	igt_assert_eq(mgl.count_objects, num_leased_obj);

	/* Get the objects */
	mgl.objects_ptr = (uint64_t) (uintptr_t) objects;

	igt_assert_eq(get_lease(lease.fd, &mgl), 0);

	/* Make sure it's 2 */
	igt_assert_eq(mgl.count_objects, num_leased_obj);

	/* Make sure we got the connector, crtc and plane back */
	for (o = 0; o < num_leased_obj; o++)
		if (objects[o] == data->connector_id)
			break;

	igt_assert_neq(o, num_leased_obj);

	for (o = 0; o < num_leased_obj; o++)
		if (objects[o] == data->crtc_id)
			break;

	igt_assert_neq(o, num_leased_obj);

	for (o = 0; o < num_leased_obj; o++)
		if (objects[o] == data->plane_id)
			break;

	igt_assert_neq(o, num_leased_obj);

	/* invalid pad */
	mgl.pad = -1;
	igt_assert_eq(get_lease(lease.fd, &mgl), -EINVAL);
	mgl.pad = 0;

	/* invalid pointer */
	mgl.objects_ptr = 0;
	igt_assert_eq(get_lease(lease.fd, &mgl), -EFAULT);

	terminate_lease(&lease);
}

static void lease_unleased_crtc(data_t *data)
{
	lease_t lease;
	enum pipe p;
	uint32_t bad_crtc_id;
	int ret;

	/* Create a valid lease */
	igt_assert_eq(make_lease(data, &lease), 0);

	igt_display_init(&lease.display, lease.fd);

	/* Find another CRTC that we don't control */
	bad_crtc_id = 0;
	for (p = 0; bad_crtc_id == 0 && p < data->master.display.n_pipes; p++) {
		if (pipe_to_crtc_id(&data->master.display, p) != data->crtc_id)
			bad_crtc_id = pipe_to_crtc_id(&data->master.display, p);
	}

	/* Give up if there isn't another crtc */
	igt_skip_on(bad_crtc_id == 0);

	/* Attempt to use the unleased crtc id. Note that the
	 * failure here is not directly from the kernel because the
	 * resources returned from the kernel will not contain this resource
	 * id and hence the igt helper functions will fail to find it
	 */
	ret = prepare_crtc(&lease, data->connector_id, bad_crtc_id);

	/* Ensure the expected error is returned */
	igt_assert_eq(ret, -ENOENT);

	terminate_lease(&lease);
}

static void lease_unleased_connector(data_t *data)
{
	lease_t lease;
	int o;
	uint32_t bad_connector_id;
	int ret;

	/* Create a valid lease */
	igt_assert_eq(make_lease(data, &lease), 0);

	igt_display_init(&lease.display, lease.fd);

	/* Find another connector that we don't control */
	bad_connector_id = 0;
	for (o = 0; bad_connector_id == 0 && o < data->master.display.n_outputs; o++) {
		if (data->master.display.outputs[o].id != data->connector_id)
			bad_connector_id = data->master.display.outputs[o].id;
	}

	/* Give up if there isn't another connector */
	igt_skip_on(bad_connector_id == 0);

	/* Attempt to use the unleased connector id. Note that the
	 * failure here is not directly from the kernel because the
	 * resources returned from the kernel will not contain this resource
	 * id and hence the igt helper functions will fail to find it
	 */
	ret = prepare_crtc(&lease, bad_connector_id, data->crtc_id);

	/* Ensure the expected error is returned */
	igt_assert_eq(ret, -ENOENT);

	terminate_lease(&lease);
}

/* Test revocation of lease */
static void lease_revoke(data_t *data)
{
	lease_t lease;
	struct local_drm_mode_revoke_lease mrl;
	int ret;

	/* Create a valid lease */
	igt_assert_eq(make_lease(data, &lease), 0);

	igt_display_init(&lease.display, lease.fd);

	/* Revoke the lease using the master fd */
	mrl.lessee_id = lease.lessee_id;
	igt_assert_eq(revoke_lease(data->master.fd, &mrl), 0);

	/* Try to use the leased objects */
	ret = prepare_crtc(&lease, data->connector_id, data->crtc_id);

	/* Ensure that the expected error is returned */
	igt_assert_eq(ret, -ENOENT);

	terminate_lease(&lease);
}

/* Test leasing objects more than once */
static void lease_again(data_t *data)
{
	lease_t lease_a, lease_b;

	/* Create a valid lease */
	igt_assert_eq(make_lease(data, &lease_a), 0);

	/* Attempt to re-lease the same objects */
	igt_assert_eq(make_lease(data, &lease_b), -EBUSY);

	terminate_lease(&lease_a);

	/* Now attempt to lease the same objects */
	igt_assert_eq(make_lease(data, &lease_b), 0);

	terminate_lease(&lease_b);
}

/* Test leasing an invalid connector */
static void lease_invalid_connector(data_t *data)
{
	lease_t lease;
	uint32_t save_connector_id;
	int ret;

	/* Create an invalid lease */
	save_connector_id = data->connector_id;
	data->connector_id = 0xbaadf00d;
	ret = make_lease(data, &lease);
	data->connector_id = save_connector_id;
	igt_assert_eq(ret, -EINVAL);
}

/* Test leasing an invalid crtc */
static void lease_invalid_crtc(data_t *data)
{
	lease_t lease;
	uint32_t save_crtc_id;
	int ret;

	/* Create an invalid lease */
	save_crtc_id = data->crtc_id;
	data->crtc_id = 0xbaadf00d;
	ret = make_lease(data, &lease);
	data->crtc_id = save_crtc_id;
	igt_assert_eq(ret, -EINVAL);
}


static void run_test(data_t *data, void (*testfunc)(data_t *))
{
	lease_t *master = &data->master;
	igt_display_t *display = &master->display;
	igt_output_t *output;
	enum pipe p;
	unsigned int valid_tests = 0;

	for_each_pipe_with_valid_output(display, p, output) {
		igt_info("Beginning %s on pipe %s, connector %s\n",
			 igt_subtest_name(),
			 kmstest_pipe_name(p),
			 igt_output_name(output));

		data->pipe = p;
		data->crtc_id = pipe_to_crtc_id(display, p);
		data->connector_id = output->id;
		data->plane_id =
			igt_pipe_get_plane_type(&data->master.display.pipes[data->pipe],
						DRM_PLANE_TYPE_PRIMARY)->drm_plane->plane_id;

		testfunc(data);

		igt_info("\n%s on pipe %s, connector %s: PASSED\n\n",
			 igt_subtest_name(),
			 kmstest_pipe_name(p),
			 igt_output_name(output));

		valid_tests++;
	}

	igt_require_f(valid_tests,
		      "no valid crtc/connector combinations found\n");
}

igt_main
{
	data_t data;
	const struct {
		const char *name;
		void (*func)(data_t *);
	} funcs[] = {
		{ "simple_lease", simple_lease },
		{ "lessee_list", lessee_list },
		{ "lease_get", lease_get },
		{ "lease_unleased_connector", lease_unleased_connector },
		{ "lease_unleased_crtc", lease_unleased_crtc },
		{ "lease_revoke", lease_revoke },
		{ "lease_again", lease_again },
		{ "lease_invalid_connector", lease_invalid_connector },
		{ "lease_invalid_crtc", lease_invalid_crtc },
		{ }
	}, *f;

	igt_skip_on_simulation();

	igt_fixture {
		data.master.fd = drm_open_driver(DRIVER_ANY);
		kmstest_set_vt_graphics_mode();
		igt_display_init(&data.master.display, data.master.fd);
	}

	for (f = funcs; f->name; f++) {

		igt_subtest_f("%s", f->name) {
			run_test(&data, f->func);
		}
	}
}
