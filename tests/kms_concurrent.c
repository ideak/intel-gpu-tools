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
 *
 */

#include "igt.h"
#include "drmtest.h"

IGT_TEST_DESCRIPTION("Test atomic mode setting concurrently with multiple planes and screen resolution");

#define SIZE_PLANE      256
#define SIZE_CURSOR     128
#define LOOP_FOREVER     -1

typedef struct {
	int drm_fd;
	igt_display_t display;
	igt_plane_t **plane;
	struct igt_fb *fb;
} data_t;

/* Command line parameters. */
struct {
	int iterations;
	unsigned int seed;
	bool user_seed;
	bool run;
} opt = {
	.iterations = 1,
	.run = true,
};

/*
 * Common code across all tests, acting on data_t
 */
static void test_init(data_t *data, enum pipe pipe, int n_planes,
		      igt_output_t *output)
{
	data->plane = calloc(n_planes, sizeof(*data->plane));
	igt_assert_f(data->plane != NULL, "Failed to allocate memory for planes\n");

	data->fb = calloc(n_planes, sizeof(struct igt_fb));
	igt_assert_f(data->fb != NULL, "Failed to allocate memory for FBs\n");
}

static void test_fini(data_t *data, enum pipe pipe, int n_planes,
		      igt_output_t *output)
{
	int i;

	for (i = 0; i < n_planes; i++) {
		igt_plane_t *plane = data->plane[i];

		if (!plane)
			continue;

		if (plane->type == DRM_PLANE_TYPE_PRIMARY)
			continue;

		igt_plane_set_fb(plane, NULL);

		data->plane[i] = NULL;
	}

	/* reset the constraint on the pipe */
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit2(&data->display, data->display.is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	free(data->plane);
	data->plane = NULL;

	free(data->fb);
	data->fb = NULL;
}

static void
create_fb_for_mode_position(data_t *data, drmModeModeInfo *mode,
			    int *rect_x, int *rect_y,
			    int *rect_w, int *rect_h,
			    uint64_t modifier, int max_planes,
			    igt_output_t *output)
{
	unsigned int fb_id;
	cairo_t *cr;
	igt_plane_t *primary;

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	fb_id = igt_create_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888,
			      modifier,
			      &data->fb[primary->index]);
	igt_assert(fb_id);

	cr = igt_get_cairo_ctx(data->drm_fd, &data->fb[primary->index]);
	igt_paint_color(cr, rect_x[0], rect_y[0],
			mode->hdisplay, mode->vdisplay,
			0.0f, 0.0f, 1.0f);

	for (int i = 0; i < max_planes; i++) {
		/*
		 * prepare_planes() skips the assignment of data->plane
		 * for primary planes as they are handled separately.
		 *
		 * Only one primary plane is assigned and prepared for
		 * the test. If there are multiple primary planes, the
		 * remaining are unassigned.
		 *
		 * Check if the data->plane is valid before accessing its
		 * members to fix this crash
		 */
		if (data->plane[i] && data->plane[i]->type == DRM_PLANE_TYPE_PRIMARY)
			continue;

		igt_paint_color(cr, rect_x[i], rect_y[i],
				rect_w[i], rect_h[i], 0.0, 0.0, 0.0);
	}

	igt_put_cairo_ctx(cr);
}

static void
prepare_planes(data_t *data, enum pipe pipe, int max_planes,
	       igt_output_t *output)
{
	drmModeModeInfo *mode;
	igt_pipe_t *p;
	igt_plane_t *primary;
	int *x;
	int *y;
	int *size;
	int i;
	int format, modifier;

	igt_output_set_pipe(output, pipe);

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	p = primary->pipe;

	x = malloc(p->n_planes * sizeof(*x));
	igt_assert_f(x, "Failed to allocate %ld bytes for variable x\n", (long int) (p->n_planes * sizeof(*x)));

	y = malloc(p->n_planes * sizeof(*y));
	igt_assert_f(y, "Failed to allocate %ld bytes for variable y\n", (long int) (p->n_planes * sizeof(*y)));

	size = malloc(p->n_planes * sizeof(*size));
	igt_assert_f(size, "Failed to allocate %ld bytes for variable size\n", (long int) (p->n_planes * sizeof(*size)));

	mode = igt_output_get_mode(output);

	/* planes with random positions */
	x[primary->index] = 0;
	y[primary->index] = 0;
	for (i = 0; i < max_planes; i++) {
		igt_plane_t *plane = igt_output_get_plane(output, i);

		if (plane->type == DRM_PLANE_TYPE_PRIMARY) {
			continue;
		} else if (plane->type == DRM_PLANE_TYPE_CURSOR) {
			size[i] = SIZE_CURSOR;
			format = DRM_FORMAT_ARGB8888;
			modifier = DRM_FORMAT_MOD_LINEAR;
		} else {
			size[i] = SIZE_PLANE;
			format = DRM_FORMAT_XRGB8888;
			modifier = DRM_FORMAT_MOD_LINEAR;
		}

		x[i] = rand() % (mode->hdisplay - size[i]);
		y[i] = rand() % (mode->vdisplay - size[i]);

		data->plane[i] = plane;

		igt_create_color_fb(data->drm_fd, size[i], size[i], format, modifier,
				    0.0f, 0.0f, 1.0f, &data->fb[i]);

		igt_plane_set_position(data->plane[i], x[i], y[i]);
		igt_plane_set_fb(data->plane[i], &data->fb[i]);
	}

	/* primary plane */
	data->plane[primary->index] = primary;
	create_fb_for_mode_position(data, mode, x, y, size, size, DRM_FORMAT_MOD_LINEAR,
				    max_planes, output);

	igt_plane_set_fb(data->plane[primary->index], &data->fb[primary->index]);

	free(x);
	free(y);
	free(size);
}

static void
test_plane_position_with_output(data_t *data, enum pipe pipe, int max_planes,
				igt_output_t *output)
{
	int i;
	int iterations = opt.iterations < 1 ? max_planes : opt.iterations;
	bool loop_forever = opt.iterations == LOOP_FOREVER ? true : false;
	int ret;

	igt_pipe_refresh(&data->display, pipe, true);

	i = 0;
	while (i < iterations || loop_forever) {
		prepare_planes(data, pipe, max_planes, output);
		ret = igt_display_try_commit2(&data->display, COMMIT_ATOMIC);

		for (int c = 0; c < max_planes; c++)
			igt_remove_fb(data->drm_fd, &data->fb[c]);

		if (ret != 0)
			max_planes > 1 ? max_planes-- : 1;

		i++;
	}
}

static drmModeModeInfo *
get_lowres_mode(data_t *data, const drmModeModeInfo *mode_default,
		igt_output_t *output)
{
	drmModeConnector *connector = output->config.connector;
	int limit = mode_default->vdisplay - SIZE_PLANE;

	if (!connector)
		return igt_std_1024_mode_get(60);

	for (int i = 0; i < connector->count_modes; i++) {
		const drmModeModeInfo *mode = &connector->modes[i];

		if (mode->vdisplay < limit)
			return igt_memdup(mode, sizeof(*mode));
	}

	igt_skip_on_f(mode_default->hdisplay < 1024, "No suitable resolution was found\n");
	return igt_std_1024_mode_get(igt_output_preferred_vrefresh(output));
}

static void
test_resolution_with_output(data_t *data, enum pipe pipe, int max_planes, igt_output_t *output)
{
	int iterations = opt.iterations < 1 ? max_planes : opt.iterations;
	bool loop_forever = opt.iterations == LOOP_FOREVER ? true : false;
	int i;

	i = 0;
	while (i < iterations || loop_forever) {
		const drmModeModeInfo *mode_hi;
		drmModeModeInfo *mode_lo;

		igt_output_set_pipe(output, pipe);

		mode_hi = igt_output_get_mode(output);
		mode_lo = get_lowres_mode(data, mode_hi, output);

		/* switch to lower resolution */
		igt_output_override_mode(output, mode_lo);
		free(mode_lo);
		if (is_amdgpu_device(data->drm_fd))
			igt_output_set_pipe(output, PIPE_NONE);
		igt_display_commit2(&data->display, COMMIT_ATOMIC);

		/* switch back to higher resolution */
		igt_output_override_mode(output, NULL);
		igt_display_commit2(&data->display, COMMIT_ATOMIC);

		i++;
	}
}

static void
run_test(data_t *data, enum pipe pipe, igt_output_t *output)
{
	int n_planes = data->display.pipes[pipe].n_planes;
	igt_display_reset(&data->display);

	if (!opt.user_seed)
		opt.seed = time(NULL);

	igt_info("Testing resolution with connector %s using pipe %s with seed %d\n",
		 igt_output_name(output), kmstest_pipe_name(pipe), opt.seed);

	srand(opt.seed);

	test_init(data, pipe, n_planes, output);

	igt_fork(child, 1) {
		test_plane_position_with_output(data, pipe, n_planes, output);
	}

	test_resolution_with_output(data, pipe, n_planes, output);

	igt_waitchildren();

	test_fini(data, pipe, n_planes, output);
}

static void
run_tests_for_pipe(data_t *data, enum pipe pipe)
{
	igt_output_t *output;

	igt_fixture {
		igt_require_pipe(&data->display, pipe);
		igt_require(data->display.pipes[pipe].n_planes > 0);

		igt_display_require_output(&data->display);
	}

	igt_describe("Test atomic mode setting concurrently with multiple planes and screen "
		     "resolution.");
	igt_subtest_with_dynamic_f("pipe-%s", kmstest_pipe_name(pipe)) {
		for_each_valid_output_on_pipe(&data->display, pipe, output)
			igt_dynamic_f("%s", igt_output_name(output))
				run_test(data, pipe, output);
	}
}

static int opt_handler(int option, int option_index, void *input)
{
	switch (option) {
	case 'i':
		opt.iterations = strtol(optarg, NULL, 0);

		if (opt.iterations < LOOP_FOREVER || opt.iterations == 0) {
			igt_info("incorrect number of iterations\n");
			igt_assert(false);
		}

		break;
	case 's':
		opt.user_seed = true;
		opt.seed = strtoul(optarg, NULL, 0);
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

const char *help_str =
	"  --iterations Number of iterations for test coverage. -1 loop forever, default 1 iteration\n"
	"  --seed       Seed for random number generator\n";
struct option long_options[] = {
	{ "iterations", required_argument, NULL, 'i'},
	{ "seed",    required_argument, NULL, 's'},
	{ 0, 0, 0, 0 }
};

static data_t data;

igt_main_args("", long_options, help_str, opt_handler, NULL)
{
	enum pipe pipe;

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		kmstest_set_vt_graphics_mode();
		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);
		if (is_i915_device(data.drm_fd))
			intel_allocator_multiprocess_start();
	}

	for_each_pipe_static(pipe) {
		igt_subtest_group
			run_tests_for_pipe(&data, pipe);
	}

	igt_fixture {
		if (is_i915_device(data.drm_fd))
			intel_allocator_multiprocess_stop();
		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
