/*
 * Copyright © 2019 Intel Corporation
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

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "drmtest.h"
#include "igt_sysfs.h"
#include "intel_chipset.h"
#include "ioctl_wrappers.h"

#include "i915/gem_engine_topology.h"

/*
 * Limit what we support for simplicity due limitation in how much we
 * can address via execbuf2.
 */
#define SIZEOF_CTX_PARAM	offsetof(struct i915_context_param_engines, \
					 engines[GEM_MAX_ENGINES])
#define SIZEOF_QUERY		offsetof(struct drm_i915_query_engine_info, \
					 engines[GEM_MAX_ENGINES])

#define DEFINE_CONTEXT_ENGINES_PARAM(e__, p__, c__, N__) \
		I915_DEFINE_CONTEXT_PARAM_ENGINES(e__, N__); \
		struct drm_i915_gem_context_param p__ = { \
			.param = I915_CONTEXT_PARAM_ENGINES, \
			.ctx_id = c__, \
			.size = SIZEOF_CTX_PARAM, \
			.value = to_user_pointer(&e__), \
		}

static int __gem_query(int fd, struct drm_i915_query *q)
{
	int err = 0;

	if (igt_ioctl(fd, DRM_IOCTL_I915_QUERY, q))
		err = -errno;

	errno = 0;
	return err;
}

static void gem_query(int fd, struct drm_i915_query *q)
{
	igt_assert_eq(__gem_query(fd, q), 0);
}

static void query_engines(int fd,
			  struct drm_i915_query_engine_info *query_engines,
			  int length)
{
	struct drm_i915_query_item item = { };
	struct drm_i915_query query = { };

	item.query_id = DRM_I915_QUERY_ENGINE_INFO;
	query.items_ptr = to_user_pointer(&item);
	query.num_items = 1;
	item.length = length;

	item.data_ptr = to_user_pointer(query_engines);

	gem_query(fd, &query);
}

static void ctx_map_engines(int fd, struct intel_engine_data *ed,
			    struct drm_i915_gem_context_param *param)
{
	struct i915_context_param_engines *engines =
			from_user_pointer(param->value);
	int i = 0;

	for (typeof(engines->engines[0]) *p =
	     &engines->engines[0];
	     i < ed->nengines; i++, p++) {
		p->engine_class = ed->engines[i].class;
		p->engine_instance = ed->engines[i].instance;
	}

	param->size = offsetof(typeof(*engines), engines[i]);
	engines->extensions = 0;

	gem_context_set_param(fd, param);
}

static const char *class_names[] = {
	[I915_ENGINE_CLASS_RENDER]	  = "rcs",
	[I915_ENGINE_CLASS_COPY]	  = "bcs",
	[I915_ENGINE_CLASS_VIDEO]	  = "vcs",
	[I915_ENGINE_CLASS_VIDEO_ENHANCE] = "vecs",
};

static void init_engine(struct intel_execution_engine2 *e2,
			uint16_t class, uint16_t instance, uint64_t flags)
{
	int ret;

	e2->class    = class;
	e2->instance = instance;

	/* engine is a virtual engine */
	if (class == I915_ENGINE_CLASS_INVALID &&
	    instance == I915_ENGINE_CLASS_INVALID_VIRTUAL) {
		strcpy(e2->name, "virtual");
		e2->is_virtual = true;
		return;
	} else {
		e2->is_virtual = false;
	}

	if (class < ARRAY_SIZE(class_names)) {
		e2->flags = flags;
		ret = snprintf(e2->name, sizeof(e2->name), "%s%u",
			       class_names[class], instance);
	} else {
		igt_debug("found unknown engine (%d, %d)\n", class, instance);
		e2->flags = -1;
		ret = snprintf(e2->name, sizeof(e2->name), "%u:%u",
			       class, instance);
	}

	igt_assert(ret < sizeof(e2->name));
}

static void query_engine_list(int fd, struct intel_engine_data *ed)
{
	uint8_t buff[SIZEOF_QUERY] = { };
	struct drm_i915_query_engine_info *query_engine =
			(struct drm_i915_query_engine_info *) buff;
	int i;

	query_engines(fd, query_engine, SIZEOF_QUERY);

	for (i = 0; i < query_engine->num_engines; i++)
		init_engine(&ed->engines[i],
			    query_engine->engines[i].engine.engine_class,
			    query_engine->engines[i].engine.engine_instance, i);

	ed->nengines = query_engine->num_engines;
}

struct intel_execution_engine2 *
intel_get_current_engine(struct intel_engine_data *ed)
{
	if (ed->n >= ed->nengines)
		ed->current_engine = NULL;
	else if (!ed->n)
		ed->current_engine = &ed->engines[0];

	return ed->current_engine;
}

void intel_next_engine(struct intel_engine_data *ed)
{
	if (ed->n + 1 < ed->nengines) {
		ed->n++;
		ed->current_engine = &ed->engines[ed->n];
	} else {
		ed->n = ed->nengines;
		ed->current_engine = NULL;
	}
}

struct intel_execution_engine2 *
intel_get_current_physical_engine(struct intel_engine_data *ed)
{
	struct intel_execution_engine2 *e;

	while ((e = intel_get_current_engine(ed)) && e->is_virtual)
	     intel_next_engine(ed);

	return e;
}

static int gem_topology_get_param(int fd,
				  struct drm_i915_gem_context_param *p)
{
	if (igt_only_list_subtests())
		return -ENODEV;

	if (__gem_context_get_param(fd, p))
		return -1; /* using default engine map */

	return 0;
}

struct intel_engine_data intel_init_engine_list(int fd, uint32_t ctx_id)
{
	DEFINE_CONTEXT_ENGINES_PARAM(engines, param, ctx_id, GEM_MAX_ENGINES);
	struct intel_engine_data engine_data = { };
	int i;

	if (gem_topology_get_param(fd, &param)) {
		/* if kernel does not support engine/context mapping */
		const struct intel_execution_engine2 *e2;

		igt_debug("using pre-allocated engine list\n");

		__for_each_static_engine(e2) {
			if (igt_only_list_subtests() ||
			    (fd < 0) ||
			    gem_has_ring(fd, e2->flags)) {
				struct intel_execution_engine2 *__e2 =
					&engine_data.engines[
					engine_data.nengines];

				strcpy(__e2->name, e2->name);
				__e2->instance   = e2->instance;
				__e2->class      = e2->class;
				__e2->flags      = e2->flags;
				__e2->is_virtual = false;

				engine_data.nengines++;
                        }
		}
		return engine_data;
	}

	if (!param.size) {
		query_engine_list(fd, &engine_data);
		ctx_map_engines(fd, &engine_data, &param);
	} else {
		/* engine count can be inferred from size */
		param.size -= sizeof(struct i915_context_param_engines);
		param.size /= sizeof(struct i915_engine_class_instance);

		igt_assert_f(param.size <= GEM_MAX_ENGINES,
			     "unsupported engine count\n");

		for (i = 0; i < param.size; i++)
			init_engine(&engine_data.engines[i],
				    engines.engines[i].engine_class,
				    engines.engines[i].engine_instance,
				    i);

		engine_data.nengines = i;
	}

	return engine_data;
}

int gem_context_lookup_engine(int fd, uint64_t engine, uint32_t ctx_id,
			      struct intel_execution_engine2 *e)
{
	DEFINE_CONTEXT_ENGINES_PARAM(engines, param, ctx_id, GEM_MAX_ENGINES);

	/* a bit paranoic */
	igt_assert(e);

	if (gem_topology_get_param(fd, &param) || !param.size)
		return -EINVAL;

	e->class = engines.engines[engine].engine_class;
	e->instance = engines.engines[engine].engine_instance;

	return 0;
}

bool gem_has_engine_topology(int fd)
{
	struct drm_i915_gem_context_param param = {
		.param = I915_CONTEXT_PARAM_ENGINES,
	};

	return !__gem_context_get_param(fd, &param);
}

struct intel_execution_engine2 gem_eb_flags_to_engine(unsigned int flags)
{
	const unsigned int ring = flags & (I915_EXEC_RING_MASK | 3 << 13);
	struct intel_execution_engine2 e2__ = {
		.class = -1,
		.instance = -1,
		.flags = -1,
	};

	if (ring == I915_EXEC_DEFAULT) {
		e2__.flags = I915_EXEC_DEFAULT;
		strcpy(e2__.name, "default");
	} else {
		const struct intel_execution_engine2 *e2;

		__for_each_static_engine(e2) {
			if (e2->flags == ring)
				return *e2;
		}

		strcpy(e2__.name, "invalid");
	}

	return e2__;
}

bool gem_context_has_engine_map(int fd, uint32_t ctx)
{
	struct drm_i915_gem_context_param param = {
		.param = I915_CONTEXT_PARAM_ENGINES,
		.ctx_id = ctx
	};

	/*
	 * If the kernel is too old to support PARAM_ENGINES,
	 * then naturally the context has no engine map.
	 */
	if (__gem_context_get_param(fd, &param))
		return false;

	return param.size;
}

bool gem_engine_is_equal(const struct intel_execution_engine2 *e1,
			 const struct intel_execution_engine2 *e2)
{
	return e1->class == e2->class && e1->instance == e2->instance;
}

static int reopen(int dir, int mode)
{
	char buf[128];
	int fd;

	snprintf(buf, sizeof(buf), "/proc/self/fd/%d", dir);
	fd = open(buf, mode);
	close(dir);

	return fd;
}

static FILE *__open_attr(int dir, const char *mode, ...)
{
	const char *path;
	FILE *file;
	va_list ap;

	va_start(ap, mode);
	while (dir >= 0 && (path = va_arg(ap, const char *))) {
		int fd;

		fd = openat(dir, path, O_RDONLY);
		close(dir);

		dir = fd;
	}
	va_end(ap);

	if (*mode != 'r') /* clumsy, but fun */
		dir = reopen(dir, O_RDWR);

	file = fdopen(dir, mode);
	if (!file) {
		close(dir);
		return NULL;
	}

	return file;
}

int gem_engine_property_scanf(int i915, const char *engine, const char *attr,
			      const char *fmt, ...)
{
	FILE *file;
	va_list ap;
	int ret;

	file = __open_attr(igt_sysfs_open(i915), "r",
			   "engine", engine, attr, NULL);
	if (!file)
		return -1;

	va_start(ap, fmt);
	ret = vfscanf(file, fmt, ap);
	va_end(ap);

	fclose(file);
	return ret;
}

int gem_engine_property_printf(int i915, const char *engine, const char *attr,
			       const char *fmt, ...)
{
	FILE *file;
	va_list ap;
	int ret;

	file = __open_attr(igt_sysfs_open(i915), "w",
			   "engine", engine, attr, NULL);
	if (!file)
		return -1;

	va_start(ap, fmt);
	ret = vfprintf(file, fmt, ap);
	va_end(ap);

	fclose(file);
	return ret;
}

uint32_t gem_engine_mmio_base(int i915, const char *engine)
{
	unsigned int mmio = 0;

	if (gem_engine_property_scanf(i915, engine, "mmio_base",
				      "%x", &mmio) < 0) {
		int gen = intel_gen(intel_get_drm_devid(i915));

		/* The layout of xcs1+ is unreliable -- hence the property! */
		if (!strcmp(engine, "rcs0")) {
			mmio = 0x2000;
		} else if (!strcmp(engine, "bcs0")) {
			mmio = 0x22000;
		} else if (!strcmp(engine, "vcs0")) {
			if (gen < 6)
				mmio = 0x4000;
			else if (gen < 11)
				mmio = 0x12000;
			else
				mmio = 0x1c0000;
		} else if (!strcmp(engine, "vecs0")) {
			if (gen < 11)
				mmio = 0x1a000;
			else
				mmio = 0x1c8000;
		}
	}

	return mmio;
}

void dyn_sysfs_engines(int i915, int engines, const char *file,
		       void (*test)(int, int))
{
	char buf[512];
	int len;

	lseek(engines, 0, SEEK_SET);
	while ((len = syscall(SYS_getdents64, engines, buf, sizeof(buf))) > 0) {
		void *ptr = buf;

		while (len) {
			struct linux_dirent64 {
				ino64_t        d_ino;
				off64_t        d_off;
				unsigned short d_reclen;
				unsigned char  d_type;
				char           d_name[];
			} *de = ptr;
			char *name;
			int engine;

			ptr += de->d_reclen;
			len -= de->d_reclen;

			engine = openat(engines, de->d_name, O_RDONLY);
			name = igt_sysfs_get(engine, "name");
			if (!name) {
				close(engine);
				continue;
			}

			igt_dynamic(name) {
				if (file) {
					struct stat st;

					igt_require(fstatat(engine, file, &st, 0) == 0);
				}

				test(i915, engine);
			}

			close(engine);
		}
	}
}
