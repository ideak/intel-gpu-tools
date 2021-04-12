/*
 * Copyright © 2018-2019 Intel Corporation
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
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/signal.h>

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/gem_vm.h"
#include "igt.h"
#include "igt_gt.h"
#include "igt_perf.h"
#include "igt_sysfs.h"
#include "sw_sync.h"

IGT_TEST_DESCRIPTION("Exercise in-kernel load-balancing");

#define MI_SEMAPHORE_WAIT		(0x1c << 23)
#define   MI_SEMAPHORE_POLL             (1 << 15)
#define   MI_SEMAPHORE_SAD_GT_SDD       (0 << 12)
#define   MI_SEMAPHORE_SAD_GTE_SDD      (1 << 12)
#define   MI_SEMAPHORE_SAD_LT_SDD       (2 << 12)
#define   MI_SEMAPHORE_SAD_LTE_SDD      (3 << 12)
#define   MI_SEMAPHORE_SAD_EQ_SDD       (4 << 12)
#define   MI_SEMAPHORE_SAD_NEQ_SDD      (5 << 12)

#define INSTANCE_COUNT (1 << I915_PMU_SAMPLE_INSTANCE_BITS)

static size_t sizeof_load_balance(int count)
{
	return offsetof(struct i915_context_engines_load_balance,
			engines[count]);
}

static size_t sizeof_param_engines(int count)
{
	return offsetof(struct i915_context_param_engines,
			engines[count]);
}

#define alloca0(sz) ({ size_t sz__ = (sz); memset(alloca(sz__), 0, sz__); })

static bool has_class_instance(int i915, uint16_t class, uint16_t instance)
{
	int fd;

	fd = perf_i915_open(i915, I915_PMU_ENGINE_BUSY(class, instance));
	if (fd >= 0) {
		close(fd);
		return true;
	}

	return false;
}

static struct i915_engine_class_instance *
list_engines(int i915, uint32_t class_mask, unsigned int *out)
{
	unsigned int count = 0, size = 64;
	struct i915_engine_class_instance *engines;

	engines = malloc(size * sizeof(*engines));
	igt_assert(engines);

	for (enum drm_i915_gem_engine_class class = I915_ENGINE_CLASS_RENDER;
	     class_mask;
	     class++, class_mask >>= 1) {
		if (!(class_mask & 1))
			continue;

		for (unsigned int instance = 0;
		     instance < INSTANCE_COUNT;
		     instance++) {
			if (!has_class_instance(i915, class, instance))
				continue;

			if (count == size) {
				size *= 2;
				engines = realloc(engines,
						  size * sizeof(*engines));
				igt_assert(engines);
			}

			engines[count++] = (struct i915_engine_class_instance){
				.engine_class = class,
				.engine_instance = instance,
			};
		}
	}

	if (!count) {
		free(engines);
		engines = NULL;
	}

	*out = count;
	return engines;
}

static bool has_perf_engines(int i915)
{
	return i915_perf_type_id(i915);
}

static int __set_vm(int i915, uint32_t ctx, uint32_t vm)
{
	struct drm_i915_gem_context_param p = {
		.ctx_id = ctx,
		.param = I915_CONTEXT_PARAM_VM,
		.value = vm
	};
	return __gem_context_set_param(i915, &p);
}

static void set_vm(int i915, uint32_t ctx, uint32_t vm)
{
	igt_assert_eq(__set_vm(i915, ctx, vm), 0);
}

static int __set_engines(int i915, uint32_t ctx,
			 const struct i915_engine_class_instance *ci,
			 unsigned int count)
{
	struct i915_context_param_engines *engines =
		alloca0(sizeof_param_engines(count));
	struct drm_i915_gem_context_param p = {
		.ctx_id = ctx,
		.param = I915_CONTEXT_PARAM_ENGINES,
		.size = sizeof_param_engines(count),
		.value = to_user_pointer(engines)
	};

	engines->extensions = 0;
	memcpy(engines->engines, ci, count * sizeof(*ci));

	return __gem_context_set_param(i915, &p);
}

static void set_engines(int i915, uint32_t ctx,
			const struct i915_engine_class_instance *ci,
			unsigned int count)
{
	igt_assert_eq(__set_engines(i915, ctx, ci, count), 0);
}

static int __set_load_balancer(int i915, uint32_t ctx,
			       const struct i915_engine_class_instance *ci,
			       unsigned int count,
			       void *ext)
{
	struct i915_context_engines_load_balance *balancer =
		alloca0(sizeof_load_balance(count));
	struct i915_context_param_engines *engines =
		alloca0(sizeof_param_engines(count + 1));
	struct drm_i915_gem_context_param p = {
		.ctx_id = ctx,
		.param = I915_CONTEXT_PARAM_ENGINES,
		.size = sizeof_param_engines(count + 1),
		.value = to_user_pointer(engines)
	};

	balancer->base.name = I915_CONTEXT_ENGINES_EXT_LOAD_BALANCE;
	balancer->base.next_extension = to_user_pointer(ext);

	igt_assert(count);
	balancer->num_siblings = count;
	memcpy(balancer->engines, ci, count * sizeof(*ci));

	engines->extensions = to_user_pointer(balancer);
	engines->engines[0].engine_class =
		I915_ENGINE_CLASS_INVALID;
	engines->engines[0].engine_instance =
		I915_ENGINE_CLASS_INVALID_NONE;
	memcpy(engines->engines + 1, ci, count * sizeof(*ci));

	return __gem_context_set_param(i915, &p);
}

static void set_load_balancer(int i915, uint32_t ctx,
			      const struct i915_engine_class_instance *ci,
			      unsigned int count,
			      void *ext)
{
	igt_assert_eq(__set_load_balancer(i915, ctx, ci, count, ext), 0);
}

static uint32_t load_balancer_create(int i915,
				     const struct i915_engine_class_instance *ci,
				     unsigned int count)
{
	uint32_t ctx;

	ctx = gem_context_create(i915);
	set_load_balancer(i915, ctx, ci, count, NULL);

	return ctx;
}

static uint32_t __batch_create(int i915, uint32_t offset)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle;

	handle = gem_create(i915, ALIGN(offset + 4, 4096));
	gem_write(i915, handle, offset, &bbe, sizeof(bbe));

	return handle;
}

static uint32_t batch_create(int i915)
{
	return __batch_create(i915, 0);
}

static void invalid_balancer(int i915)
{
	I915_DEFINE_CONTEXT_ENGINES_LOAD_BALANCE(balancer, 64);
	I915_DEFINE_CONTEXT_PARAM_ENGINES(engines, 64);
	struct drm_i915_gem_context_param p = {
		.param = I915_CONTEXT_PARAM_ENGINES,
		.value = to_user_pointer(&engines)
	};
	uint32_t handle;
	void *ptr;

	/*
	 * Assume that I915_CONTEXT_PARAM_ENGINE validates the array
	 * of engines[], our job is to determine if the load_balancer
	 * extension explodes.
	 */

	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *ci;
		unsigned int count;

		ci = list_engines(i915, 1 << class, &count);
		if (!ci)
			continue;

		igt_assert_lte(count, 64);

		p.ctx_id = gem_context_create(i915);
		p.size = (sizeof(struct i915_context_param_engines) +
			  (count + 1) * sizeof(*engines.engines));

		memset(&engines, 0, sizeof(engines));
		engines.engines[0].engine_class = I915_ENGINE_CLASS_INVALID;
		engines.engines[0].engine_instance = I915_ENGINE_CLASS_INVALID_NONE;
		memcpy(engines.engines + 1, ci, count * sizeof(*ci));
		gem_context_set_param(i915, &p);

		engines.extensions = -1ull;
		igt_assert_eq(__gem_context_set_param(i915, &p), -EFAULT);

		engines.extensions = 1ull;
		igt_assert_eq(__gem_context_set_param(i915, &p), -EFAULT);

		memset(&balancer, 0, sizeof(balancer));
		balancer.base.name = I915_CONTEXT_ENGINES_EXT_LOAD_BALANCE;
		balancer.num_siblings = count;
		memcpy(balancer.engines, ci, count * sizeof(*ci));

		engines.extensions = to_user_pointer(&balancer);
		gem_context_set_param(i915, &p);

		balancer.engine_index = 1;
		igt_assert_eq(__gem_context_set_param(i915, &p), -EEXIST);

		balancer.engine_index = count;
		igt_assert_eq(__gem_context_set_param(i915, &p), -EEXIST);

		balancer.engine_index = count + 1;
		igt_assert_eq(__gem_context_set_param(i915, &p), -EINVAL);

		balancer.engine_index = 0;
		gem_context_set_param(i915, &p);

		balancer.base.next_extension = to_user_pointer(&balancer);
		igt_assert_eq(__gem_context_set_param(i915, &p), -EEXIST);

		balancer.base.next_extension = -1ull;
		igt_assert_eq(__gem_context_set_param(i915, &p), -EFAULT);

		handle = gem_create(i915, 4096 * 3);
		ptr = gem_mmap__device_coherent(i915, handle, 0, 4096 * 3,
						PROT_WRITE);
		gem_close(i915, handle);

		memset(&engines, 0, sizeof(engines));
		engines.engines[0].engine_class = I915_ENGINE_CLASS_INVALID;
		engines.engines[0].engine_instance = I915_ENGINE_CLASS_INVALID_NONE;
		engines.engines[1].engine_class = I915_ENGINE_CLASS_INVALID;
		engines.engines[1].engine_instance = I915_ENGINE_CLASS_INVALID_NONE;
		memcpy(engines.engines + 2, ci, count * sizeof(ci));
		p.size = (sizeof(struct i915_context_param_engines) +
			  (count + 2) * sizeof(*engines.engines));
		gem_context_set_param(i915, &p);

		balancer.base.next_extension = 0;
		balancer.engine_index = 1;
		engines.extensions = to_user_pointer(&balancer);
		gem_context_set_param(i915, &p);

		memcpy(ptr + 4096 - 8, &balancer, sizeof(balancer));
		memcpy(ptr + 8192 - 8, &balancer, sizeof(balancer));
		balancer.engine_index = 0;

		engines.extensions = to_user_pointer(ptr) + 4096 - 8;
		gem_context_set_param(i915, &p);

		balancer.base.next_extension = engines.extensions;
		engines.extensions = to_user_pointer(&balancer);
		gem_context_set_param(i915, &p);

		munmap(ptr, 4096);
		igt_assert_eq(__gem_context_set_param(i915, &p), -EFAULT);
		engines.extensions = to_user_pointer(ptr) + 4096 - 8;
		igt_assert_eq(__gem_context_set_param(i915, &p), -EFAULT);

		engines.extensions = to_user_pointer(ptr) + 8192 - 8;
		gem_context_set_param(i915, &p);

		balancer.base.next_extension = engines.extensions;
		engines.extensions = to_user_pointer(&balancer);
		gem_context_set_param(i915, &p);

		munmap(ptr + 8192, 4096);
		igt_assert_eq(__gem_context_set_param(i915, &p), -EFAULT);
		engines.extensions = to_user_pointer(ptr) + 8192 - 8;
		igt_assert_eq(__gem_context_set_param(i915, &p), -EFAULT);

		munmap(ptr + 4096, 4096);

		gem_context_destroy(i915, p.ctx_id);
		free(ci);
	}
}

static void invalid_bonds(int i915)
{
	I915_DEFINE_CONTEXT_ENGINES_BOND(bonds[16], 1);
	I915_DEFINE_CONTEXT_PARAM_ENGINES(engines, 1);
	struct drm_i915_gem_context_param p = {
		.ctx_id = gem_context_create(i915),
		.param = I915_CONTEXT_PARAM_ENGINES,
		.value = to_user_pointer(&engines),
		.size = sizeof(engines),
	};
	uint32_t handle;
	void *ptr;

	memset(&engines, 0, sizeof(engines));
	gem_context_set_param(i915, &p);

	memset(bonds, 0, sizeof(bonds));
	for (int n = 0; n < ARRAY_SIZE(bonds); n++) {
		bonds[n].base.name = I915_CONTEXT_ENGINES_EXT_BOND;
		bonds[n].base.next_extension =
			n ? to_user_pointer(&bonds[n - 1]) : 0;
		bonds[n].num_bonds = 1;
	}
	engines.extensions = to_user_pointer(&bonds);
	gem_context_set_param(i915, &p);

	bonds[0].base.next_extension = -1ull;
	igt_assert_eq(__gem_context_set_param(i915, &p), -EFAULT);

	bonds[0].base.next_extension = to_user_pointer(&bonds[0]);
	igt_assert_eq(__gem_context_set_param(i915, &p), -E2BIG);

	engines.extensions = to_user_pointer(&bonds[1]);
	igt_assert_eq(__gem_context_set_param(i915, &p), -E2BIG);
	bonds[0].base.next_extension = 0;
	gem_context_set_param(i915, &p);

	handle = gem_create(i915, 4096 * 3);
	ptr = gem_mmap__device_coherent(i915, handle, 0, 4096 * 3, PROT_WRITE);
	gem_close(i915, handle);

	memcpy(ptr + 4096, &bonds[0], sizeof(bonds[0]));
	engines.extensions = to_user_pointer(ptr) + 4096;
	gem_context_set_param(i915, &p);

	memcpy(ptr, &bonds[0], sizeof(bonds[0]));
	bonds[0].base.next_extension = to_user_pointer(ptr);
	memcpy(ptr + 4096, &bonds[0], sizeof(bonds[0]));
	gem_context_set_param(i915, &p);

	munmap(ptr, 4096);
	igt_assert_eq(__gem_context_set_param(i915, &p), -EFAULT);

	bonds[0].base.next_extension = 0;
	memcpy(ptr + 8192, &bonds[0], sizeof(bonds[0]));
	bonds[0].base.next_extension = to_user_pointer(ptr) + 8192;
	memcpy(ptr + 4096, &bonds[0], sizeof(bonds[0]));
	gem_context_set_param(i915, &p);

	munmap(ptr + 8192, 4096);
	igt_assert_eq(__gem_context_set_param(i915, &p), -EFAULT);

	munmap(ptr + 4096, 4096);
	igt_assert_eq(__gem_context_set_param(i915, &p), -EFAULT);

	gem_context_destroy(i915, p.ctx_id);
}

static void kick_kthreads(void)
{
	usleep(20 * 1000); /* 20ms should be enough for ksoftirqd! */
}

static double measure_load(int pmu, int period_us)
{
	uint64_t data[2];
	uint64_t d_t, d_v;

	kick_kthreads();

	igt_assert_eq(read(pmu, data, sizeof(data)), sizeof(data));
	d_v = -data[0];
	d_t = -data[1];

	usleep(period_us);

	igt_assert_eq(read(pmu, data, sizeof(data)), sizeof(data));
	d_v += data[0];
	d_t += data[1];

	return d_v / (double)d_t;
}

static double measure_min_load(int pmu, unsigned int num, int period_us)
{
	uint64_t data[2 + num];
	uint64_t d_t, d_v[num];
	uint64_t min = -1, max = 0;

	kick_kthreads();

	igt_assert_eq(read(pmu, data, sizeof(data)), sizeof(data));
	for (unsigned int n = 0; n < num; n++)
		d_v[n] = -data[2 + n];
	d_t = -data[1];

	usleep(period_us);

	igt_assert_eq(read(pmu, data, sizeof(data)), sizeof(data));

	d_t += data[1];
	for (unsigned int n = 0; n < num; n++) {
		d_v[n] += data[2 + n];
		igt_debug("engine[%d]: %.1f%%\n",
			  n, d_v[n] / (double)d_t * 100);
		if (d_v[n] < min)
			min = d_v[n];
		if (d_v[n] > max)
			max = d_v[n];
	}

	igt_debug("elapsed: %"PRIu64"ns, load [%.1f, %.1f]%%\n",
		  d_t, min / (double)d_t * 100,  max / (double)d_t * 100);

	return min / (double)d_t;
}

static void measure_all_load(int pmu, double *v, unsigned int num, int period_us)
{
	uint64_t data[2 + num];
	uint64_t d_t, d_v[num];

	kick_kthreads();

	igt_assert_eq(read(pmu, data, sizeof(data)), sizeof(data));
	for (unsigned int n = 0; n < num; n++)
		d_v[n] = -data[2 + n];
	d_t = -data[1];

	usleep(period_us);

	igt_assert_eq(read(pmu, data, sizeof(data)), sizeof(data));

	d_t += data[1];
	for (unsigned int n = 0; n < num; n++) {
		d_v[n] += data[2 + n];
		igt_debug("engine[%d]: %.1f%%\n",
			  n, d_v[n] / (double)d_t * 100);
		v[n] = d_v[n] / (double)d_t;
	}
}

static int
add_pmu(int i915, int pmu, const struct i915_engine_class_instance *ci)
{
	return perf_i915_open_group(i915,
				    I915_PMU_ENGINE_BUSY(ci->engine_class,
							 ci->engine_instance),
				    pmu);
}

static const char *class_to_str(int class)
{
	const char *str[] = {
		[I915_ENGINE_CLASS_RENDER] = "rcs",
		[I915_ENGINE_CLASS_COPY] = "bcs",
		[I915_ENGINE_CLASS_VIDEO] = "vcs",
		[I915_ENGINE_CLASS_VIDEO_ENHANCE] = "vecs",
	};

	if (class < ARRAY_SIZE(str))
		return str[class];

	return "unk";
}

static void check_individual_engine(int i915,
				    uint32_t ctx,
				    const struct i915_engine_class_instance *ci,
				    int idx)
{
	igt_spin_t *spin;
	double load;
	int pmu;

	pmu = perf_i915_open(i915,
			     I915_PMU_ENGINE_BUSY(ci[idx].engine_class,
						  ci[idx].engine_instance));

	spin = igt_spin_new(i915, .ctx_id = ctx, .engine = idx + 1);
	load = measure_load(pmu, 10000);
	igt_spin_free(i915, spin);

	close(pmu);

	igt_assert_f(load > 0.90,
		     "engine %d (class:instance %d:%d) was found to be only %.1f%% busy\n",
		     idx, ci[idx].engine_class, ci[idx].engine_instance, load*100);
}

static void individual(int i915)
{
	/*
	 * I915_CONTEXT_PARAM_ENGINE allows us to index into the user
	 * supplied array from gem_execbuf(). Our check is to build the
	 * ctx->engine[] with various different engine classes, feed in
	 * a spinner and then ask pmu to confirm it the expected engine
	 * was busy.
	 */

	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *ci;
		unsigned int count;

		ci = list_engines(i915, 1u << class, &count);
		if (!ci)
			continue;

		for (int pass = 0; pass < count; pass++) { /* approx. count! */
			uint32_t ctx;

			igt_assert(sizeof(*ci) == sizeof(int));
			igt_permute_array(ci, count, igt_exchange_int);
			ctx = gem_context_create(i915);
			set_load_balancer(i915, ctx, ci, count, NULL);
			for (unsigned int n = 0; n < count; n++)
				check_individual_engine(i915, ctx, ci, n);
			gem_context_destroy(i915, ctx);
		}

		free(ci);
	}

	gem_quiescent_gpu(i915);
}

static void bonded(int i915, unsigned int flags)
#define CORK 0x1
{
	I915_DEFINE_CONTEXT_ENGINES_BOND(bonds[16], 1);
	struct i915_engine_class_instance *master_engines;
	uint32_t vm;

	/*
	 * I915_CONTEXT_PARAM_ENGINE provides an extension that allows us
	 * to specify which engine(s) to pair with a parallel (EXEC_SUBMIT)
	 * request submitted to another engine.
	 */

	vm = gem_vm_create(i915);

	memset(bonds, 0, sizeof(bonds));
	for (int n = 0; n < ARRAY_SIZE(bonds); n++) {
		bonds[n].base.name = I915_CONTEXT_ENGINES_EXT_BOND;
		bonds[n].base.next_extension =
			n ? to_user_pointer(&bonds[n - 1]) : 0;
		bonds[n].num_bonds = 1;
	}

	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *siblings;
		unsigned int count, limit, *order;
		uint32_t master, ctx;
		int n;

		siblings = list_engines(i915, 1u << class, &count);
		if (!siblings)
			continue;

		if (count < 2) {
			free(siblings);
			continue;
		}

		master_engines = list_engines(i915, ~(1u << class), &limit);
		master = gem_context_create_ext(i915, I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE, 0);
		set_vm(i915, master, vm);
		set_engines(i915, master, master_engines, limit);

		limit = min(count, limit);
		igt_assert(limit <= ARRAY_SIZE(bonds));
		for (n = 0; n < limit; n++) {
			bonds[n].master = master_engines[n];
			bonds[n].engines[0] = siblings[n];
		}

		ctx = gem_context_create_ext(i915, I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE, 0);
		set_vm(i915, ctx, vm);
		set_engines(i915, ctx, master_engines, limit);
		set_load_balancer(i915, ctx, siblings, count, &bonds[limit - 1]);

		order = malloc(sizeof(*order) * 8 * limit);
		igt_assert(order);
		for (n = 0; n < limit; n++)
			order[2 * limit - n - 1] = order[n] = n % limit;
		memcpy(order + 2 * limit, order, 2 * limit * sizeof(*order));
		memcpy(order + 4 * limit, order, 4 * limit * sizeof(*order));
		igt_permute_array(order + 2 * limit, 6 * limit, igt_exchange_int);

		for (n = 0; n < 8 * limit; n++) {
			struct drm_i915_gem_execbuffer2 eb;
			igt_spin_t *spin, *plug;
			IGT_CORK_HANDLE(cork);
			double v[limit];
			int pmu[limit + 1];
			int bond = order[n];

			pmu[0] = -1;
			for (int i = 0; i < limit; i++)
				pmu[i] = add_pmu(i915, pmu[0], &siblings[i]);
			pmu[limit] = add_pmu(i915,
					     pmu[0], &master_engines[bond]);

			igt_assert(siblings[bond].engine_class !=
				   master_engines[bond].engine_class);

			plug = NULL;
			if (flags & CORK) {
				plug = __igt_spin_new(i915,
						      .ctx_id = master,
						      .engine = bond,
						      .dependency = igt_cork_plug(&cork, i915));
			}

			spin = __igt_spin_new(i915,
					      .ctx_id = master,
					      .engine = bond,
					      .flags = IGT_SPIN_FENCE_OUT);

			eb = spin->execbuf;
			eb.rsvd1 = ctx;
			eb.rsvd2 = spin->out_fence;
			eb.flags = I915_EXEC_FENCE_SUBMIT;
			gem_execbuf(i915, &eb);

			if (plug) {
				igt_cork_unplug(&cork);
				igt_spin_free(i915, plug);
			}

			measure_all_load(pmu[0], v, limit + 1, 10000);
			igt_spin_free(i915, spin);

			igt_assert_f(v[bond] > 0.90,
				     "engine %d (class:instance %s:%d) was found to be only %.1f%% busy\n",
				     bond,
				     class_to_str(siblings[bond].engine_class),
				     siblings[bond].engine_instance,
				     100 * v[bond]);
			for (int other = 0; other < limit; other++) {
				if (other == bond)
					continue;

				igt_assert_f(v[other] == 0,
					     "engine %d (class:instance %s:%d) was not idle, and actually %.1f%% busy\n",
					     other,
					     class_to_str(siblings[other].engine_class),
					     siblings[other].engine_instance,
					     100 * v[other]);
			}
			igt_assert_f(v[limit] > 0.90,
				     "master (class:instance %s:%d) was found to be only %.1f%% busy\n",
				     class_to_str(master_engines[bond].engine_class),
				     master_engines[bond].engine_instance,
				     100 * v[limit]);

			close(pmu[0]);
		}

		free(order);
		gem_context_destroy(i915, master);
		gem_context_destroy(i915, ctx);
		free(master_engines);
		free(siblings);
	}
}

#define VIRTUAL_ENGINE (1u << 0)

static unsigned int offset_in_page(void *addr)
{
	return (uintptr_t)addr & 4095;
}

static uint32_t create_semaphore_to_spinner(int i915, igt_spin_t *spin)
{
	uint32_t *cs, *map;
	uint32_t handle;
	uint64_t addr;

	handle = gem_create(i915, 4096);
	cs = map = gem_mmap__device_coherent(i915, handle, 0, 4096, PROT_WRITE);

	/* Wait until the spinner is running */
	addr = spin->obj[0].offset + 4 * SPIN_POLL_START_IDX;
	*cs++ = MI_SEMAPHORE_WAIT |
		MI_SEMAPHORE_POLL |
		MI_SEMAPHORE_SAD_NEQ_SDD |
		(4 - 2);
	*cs++ = 0;
	*cs++ = addr;
	*cs++ = addr >> 32;

	/* Then cancel the spinner */
	addr = spin->obj[IGT_SPIN_BATCH].offset +
		offset_in_page(spin->condition);
	*cs++ = MI_STORE_DWORD_IMM;
	*cs++ = addr;
	*cs++ = addr >> 32;
	*cs++ = MI_BATCH_BUFFER_END;

	*cs++ = MI_BATCH_BUFFER_END;
	munmap(map, 4096);

	return handle;
}

static void bonded_slice(int i915)
{
	int *stop;

	/*
	 * Mix and match bonded/parallel execution of multiple requests in
	 * the presence of background load and timeslicing [preemption].
	 */

	igt_require(gem_scheduler_has_semaphores(i915));

	stop = mmap(0, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(stop != MAP_FAILED);

	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *siblings;
		struct drm_i915_gem_exec_object2 obj[3] = {};
		struct drm_i915_gem_execbuffer2 eb = {};
		unsigned int count;
		uint32_t ctx;
		igt_spin_t *spin;

		siblings = list_engines(i915, 1u << class, &count);
		if (!siblings)
			continue;

		if (count < 2) {
			free(siblings);
			continue;
		}

		/*
		 * A: semaphore wait on spinner on a real engine; cancel spinner
		 * B: unpreemptable spinner on virtual engine
		 *
		 * A waits for running ack from B, if scheduled on the same
		 * engine -> hang.
		 *
		 * C+: background load across engines to trigger timeslicing
		 *
		 * XXX add explicit bonding options for A->B
		 */

		ctx = gem_context_create(i915); /* NB timeline per engine */
		set_load_balancer(i915, ctx, siblings, count, NULL);

		spin = __igt_spin_new(i915,
				      .ctx_id = ctx,
				      .flags = (IGT_SPIN_NO_PREEMPTION |
						IGT_SPIN_POLL_RUN));
		igt_spin_end(spin); /* we just want its address for later */
		gem_sync(i915, spin->handle);
		igt_spin_reset(spin);

		/* igt_spin_t poll and batch obj must be laid out as we expect */
		igt_assert_eq(IGT_SPIN_BATCH, 1);
		obj[0] = spin->obj[0];
		obj[1] = spin->obj[1];
		obj[2].handle = create_semaphore_to_spinner(i915, spin);

		eb.buffers_ptr = to_user_pointer(obj);
		eb.rsvd1 = ctx;

		*stop = 0;
		igt_fork(child, count + 1) { /* C: arbitrary background load */
			igt_list_del(&spin->link);

			ctx = load_balancer_create(i915, siblings, count);

			while (!READ_ONCE(*stop)) {
				spin = igt_spin_new(i915,
						    .ctx_id = ctx,
						    .engine = (1 + rand() % count),
						    .flags = IGT_SPIN_POLL_RUN);
				igt_spin_busywait_until_started(spin);
				usleep(50000);
				igt_spin_free(i915, spin);
			}

			gem_context_destroy(i915, ctx);
		}

		igt_until_timeout(5) {
			igt_spin_reset(spin); /* indirectly cancelled by A */

			/* A: Submit the semaphore wait on a real engine */
			eb.buffer_count = 3;
			eb.flags = (1 + rand() % count) | I915_EXEC_FENCE_OUT;
			gem_execbuf_wr(i915, &eb);

			/* B: Submit the spinner (in parallel) on virtual [0] */
			eb.buffer_count = 2;
			eb.flags = 0 | I915_EXEC_FENCE_SUBMIT;
			eb.rsvd2 >>= 32;
			gem_execbuf(i915, &eb);
			close(eb.rsvd2);

			gem_sync(i915, obj[0].handle);
		}

		*stop = 1;
		igt_waitchildren();

		gem_close(i915, obj[2].handle);
		igt_spin_free(i915, spin);
		gem_context_destroy(i915, ctx);
	}

	munmap(stop, 4096);
}

static void __bonded_chain(int i915,
			   const struct i915_engine_class_instance *siblings,
			   unsigned int count)
{
	const int priorities[] = { -1023, 0, 1023 };
	struct drm_i915_gem_exec_object2 batch = {
		.handle = batch_create(i915),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&batch),
		.buffer_count = 1,
	};
	igt_spin_t *spin;

	for (int i = 0; i < ARRAY_SIZE(priorities); i++) {
		uint32_t ctx;
		/* A: spin forever on engine 1 */

		ctx = gem_context_create(i915);
		set_load_balancer(i915, ctx, siblings, count, NULL);
		if (priorities[i] < 0)
			gem_context_set_priority(i915, ctx, priorities[i]);
		spin = igt_spin_new(i915,
				    .ctx_id = ctx,
				    .engine = 1,
				    .flags = (IGT_SPIN_POLL_RUN |
					      IGT_SPIN_FENCE_OUT));
		igt_spin_busywait_until_started(spin);

		/*
		 * Note we replace the contexts and their timelines between
		 * each execbuf, so that any pair of requests on the same
		 * engine could be re-ordered by the scheduler -- if the
		 * dependency tracking is subpar.
		 */

		/* B: waits for A on engine 2 */
		gem_context_destroy(i915, ctx);
		ctx = gem_context_create(i915);
		gem_context_set_priority(i915, ctx, 0);
		set_load_balancer(i915, ctx, siblings, count, NULL);
		execbuf.rsvd1 = ctx;
		execbuf.rsvd2 = spin->out_fence;
		execbuf.flags = I915_EXEC_FENCE_IN | I915_EXEC_FENCE_OUT;
		execbuf.flags |= 2; /* opposite engine to spinner */
		gem_execbuf_wr(i915, &execbuf);

		/* B': run in parallel with B on engine 1, i.e. not before A! */
		if (priorities[i] > 0)
			gem_context_set_priority(i915, ctx, priorities[i]);
		execbuf.flags = I915_EXEC_FENCE_SUBMIT | I915_EXEC_FENCE_OUT;
		execbuf.flags |= 1; /* same engine as spinner */
		execbuf.rsvd2 >>= 32;
		gem_execbuf_wr(i915, &execbuf);
		gem_context_set_priority(i915, ctx, 0);

		/* Wait for any magic timeslicing or preemptions... */
		igt_assert_eq(sync_fence_wait(execbuf.rsvd2 >> 32, 1000),
			      -ETIME);

		igt_debugfs_dump(i915, "i915_engine_info");

		/*
		 * ... which should not have happened, so everything is still
		 * waiting on the spinner
		 */
		igt_assert_eq(sync_fence_status(spin->out_fence), 0);
		igt_assert_eq(sync_fence_status(execbuf.rsvd2 & 0xffffffff), 0);
		igt_assert_eq(sync_fence_status(execbuf.rsvd2 >> 32), 0);

		igt_spin_free(i915, spin);
		gem_context_destroy(i915, ctx);
		gem_sync(i915, batch.handle);

		igt_assert_eq(sync_fence_status(execbuf.rsvd2 & 0xffffffff), 1);
		igt_assert_eq(sync_fence_status(execbuf.rsvd2 >> 32), 1);

		close(execbuf.rsvd2);
		close(execbuf.rsvd2 >> 32);
	}

	gem_close(i915, batch.handle);
}

static void __bonded_chain_inv(int i915,
			       const struct i915_engine_class_instance *siblings,
			       unsigned int count)
{
	const int priorities[] = { -1023, 0, 1023 };
	struct drm_i915_gem_exec_object2 batch = {
		.handle = batch_create(i915),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&batch),
		.buffer_count = 1,
	};
	igt_spin_t *spin;

	for (int i = 0; i < ARRAY_SIZE(priorities); i++) {
		uint32_t ctx;

		/* A: spin forever on engine 1 */
		ctx = gem_context_create(i915);
		set_load_balancer(i915, ctx, siblings, count, NULL);
		if (priorities[i] < 0)
			gem_context_set_priority(i915, ctx, priorities[i]);
		spin = igt_spin_new(i915,
				    .ctx_id = ctx,
				    .engine = 1,
				    .flags = (IGT_SPIN_POLL_RUN |
					      IGT_SPIN_FENCE_OUT));
		igt_spin_busywait_until_started(spin);

		/* B: waits for A on engine 1 */
		gem_context_destroy(i915, ctx);
		ctx = gem_context_create(i915);
		gem_context_set_priority(i915, ctx, 0);
		set_load_balancer(i915, ctx, siblings, count, NULL);
		execbuf.rsvd1 = ctx;
		execbuf.rsvd2 = spin->out_fence;
		execbuf.flags = I915_EXEC_FENCE_IN | I915_EXEC_FENCE_OUT;
		execbuf.flags |= 1; /* same engine as spinner */
		gem_execbuf_wr(i915, &execbuf);

		/* B': run in parallel with B on engine 2, i.e. not before A! */
		if (priorities[i] > 0)
			gem_context_set_priority(i915, ctx, priorities[i]);
		execbuf.flags = I915_EXEC_FENCE_SUBMIT | I915_EXEC_FENCE_OUT;
		execbuf.flags |= 2; /* opposite engine to spinner */
		execbuf.rsvd2 >>= 32;
		gem_execbuf_wr(i915, &execbuf);
		gem_context_set_priority(i915, ctx, 0);

		/* Wait for any magic timeslicing or preemptions... */
		igt_assert_eq(sync_fence_wait(execbuf.rsvd2 >> 32, 1000),
			      -ETIME);

		igt_debugfs_dump(i915, "i915_engine_info");

		/*
		 * ... which should not have happened, so everything is still
		 * waiting on the spinner
		 */
		igt_assert_eq(sync_fence_status(spin->out_fence), 0);
		igt_assert_eq(sync_fence_status(execbuf.rsvd2 & 0xffffffff), 0);
		igt_assert_eq(sync_fence_status(execbuf.rsvd2 >> 32), 0);

		igt_spin_free(i915, spin);
		gem_sync(i915, batch.handle);
		gem_context_destroy(i915, ctx);

		igt_assert_eq(sync_fence_status(execbuf.rsvd2 & 0xffffffff), 1);
		igt_assert_eq(sync_fence_status(execbuf.rsvd2 >> 32), 1);

		close(execbuf.rsvd2);
		close(execbuf.rsvd2 >> 32);
	}

	gem_close(i915, batch.handle);
}

static void bonded_chain(int i915)
{
	/*
	 * Given batches A, B and B', where B and B' are a bonded pair, with
	 * B' depending on B with a submit fence and B depending on A as
	 * an ordinary fence; prove B' cannot complete before A.
	 */

	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *siblings;
		unsigned int count;

		siblings = list_engines(i915, 1u << class, &count);
		if (count > 1) {
			__bonded_chain(i915, siblings, count);
			__bonded_chain_inv(i915, siblings, count);
		}
		free(siblings);
	}
}

static void __bonded_sema(int i915,
			  const struct i915_engine_class_instance *siblings,
			  unsigned int count)
{
	const int priorities[] = { -1023, 0, 1023 };
	struct drm_i915_gem_exec_object2 batch = {
		.handle = batch_create(i915),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&batch),
		.buffer_count = 1,
	};
	igt_spin_t *spin;

	for (int i = 0; i < ARRAY_SIZE(priorities); i++) {
		uint32_t ctx;

		/* A: spin forever on seperate render engine */
		spin = igt_spin_new(i915,
				    .flags = (IGT_SPIN_POLL_RUN |
					      IGT_SPIN_FENCE_OUT));
		igt_spin_busywait_until_started(spin);

		/*
		 * Note we replace the contexts and their timelines between
		 * each execbuf, so that any pair of requests on the same
		 * engine could be re-ordered by the scheduler -- if the
		 * dependency tracking is subpar.
		 */

		/* B: waits for A (using a semaphore) on engine 1 */
		ctx = gem_context_create(i915);
		set_load_balancer(i915, ctx, siblings, count, NULL);
		execbuf.rsvd1 = ctx;
		execbuf.rsvd2 = spin->out_fence;
		execbuf.flags = I915_EXEC_FENCE_IN | I915_EXEC_FENCE_OUT;
		execbuf.flags |= 1;
		gem_execbuf_wr(i915, &execbuf);

		/* B': run in parallel with B on engine 2 */
		gem_context_destroy(i915, ctx);
		ctx = gem_context_create(i915);
		if (priorities[i] > 0)
			gem_context_set_priority(i915, ctx, priorities[i]);
		set_load_balancer(i915, ctx, siblings, count, NULL);
		execbuf.rsvd1 = ctx;
		execbuf.flags = I915_EXEC_FENCE_SUBMIT | I915_EXEC_FENCE_OUT;
		execbuf.flags |= 2;
		execbuf.rsvd2 >>= 32;
		gem_execbuf_wr(i915, &execbuf);
		gem_context_set_priority(i915, ctx, 0);

		/* Wait for any magic timeslicing or preemptions... */
		igt_assert_eq(sync_fence_wait(execbuf.rsvd2 >> 32, 1000),
			      -ETIME);

		igt_debugfs_dump(i915, "i915_engine_info");

		/*
		 * ... which should not have happened, so everything is still
		 * waiting on the spinner
		 */
		igt_assert_eq(sync_fence_status(spin->out_fence), 0);
		igt_assert_eq(sync_fence_status(execbuf.rsvd2 & 0xffffffff), 0);
		igt_assert_eq(sync_fence_status(execbuf.rsvd2 >> 32), 0);

		igt_spin_free(i915, spin);
		gem_sync(i915, batch.handle);
		gem_context_destroy(i915, ctx);

		igt_assert_eq(sync_fence_status(execbuf.rsvd2 & 0xffffffff), 1);
		igt_assert_eq(sync_fence_status(execbuf.rsvd2 >> 32), 1);

		close(execbuf.rsvd2);
		close(execbuf.rsvd2 >> 32);
	}

	gem_close(i915, batch.handle);
}

static void bonded_semaphore(int i915)
{
	/*
	 * Given batches A, B and B', where B and B' are a bonded pair, with
	 * B' depending on B with a submit fence and B depending on A as
	 * an ordinary fence; prove B' cannot complete before A, with the
	 * difference here (wrt bonded_chain) that A is on another engine and
	 * so A, B and B' are expected to be inflight concurrently.
	 */
	igt_require(gem_scheduler_has_semaphores(i915));

	for (int class = 1; class < 32; class++) {
		struct i915_engine_class_instance *siblings;
		unsigned int count;

		siblings = list_engines(i915, 1u << class, &count);
		if (count > 1)
			__bonded_sema(i915, siblings, count);
		free(siblings);
	}
}

static void __bonded_pair(int i915,
			  const struct i915_engine_class_instance *siblings,
			  unsigned int count,
			  unsigned int flags,
			  unsigned long *out)
#define B_FENCE 0x1
#define B_HOSTILE 0x2
#define B_MANY 0x4
#define B_DELAY 0x8
{
	struct drm_i915_gem_exec_object2 batch = {};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&batch),
		.buffer_count = 1,
	};
	unsigned long cycles = 0;
	unsigned int spinner;
	igt_spin_t *a;
	int timeline;
	uint32_t A;

	srandom(getpid());

	spinner = IGT_SPIN_POLL_RUN;
	if (flags & B_HOSTILE)
		spinner |= IGT_SPIN_NO_PREEMPTION;

	A = gem_context_create(i915);
	set_load_balancer(i915, A, siblings, count, NULL);
	a = igt_spin_new(i915, A, .flags = spinner);
	igt_spin_end(a);
	gem_sync(i915, a->handle);

	timeline = sw_sync_timeline_create();

	igt_until_timeout(2) {
		unsigned int master;
		int fence;

		master = 1;
		if (flags & B_MANY)
			master = rand() % count + 1;

		fence = -1;
		if (flags & B_FENCE)
			fence = sw_sync_timeline_create_fence(timeline,
							      cycles + 1);

		igt_spin_reset(a);
		a->execbuf.flags = master | I915_EXEC_FENCE_OUT;
		if (fence != -1) {
			a->execbuf.rsvd2 = fence;
			a->execbuf.flags |= I915_EXEC_FENCE_IN;
		}
		gem_execbuf_wr(i915, &a->execbuf);

		if (flags & B_DELAY)
			usleep(100);

		batch.handle = create_semaphore_to_spinner(i915, a);
		execbuf.rsvd1 = a->execbuf.rsvd1;
		execbuf.rsvd2 = a->execbuf.rsvd2 >> 32;
		do {
			execbuf.flags = rand() % count + 1;
		} while (execbuf.flags == master);
		execbuf.flags |= I915_EXEC_FENCE_SUBMIT;
		gem_execbuf(i915, &execbuf);
		gem_close(i915, batch.handle);

		if (fence != -1) {
			sw_sync_timeline_inc(timeline, 1);
			close(fence);
		}
		close(a->execbuf.rsvd2 >> 32);

		gem_sync(i915, a->handle);

		cycles++;
	}

	close(timeline);
	igt_spin_free(i915, a);
	gem_context_destroy(i915, A);

	*out = cycles;
}

static void __bonded_dual(int i915,
			  const struct i915_engine_class_instance *siblings,
			  unsigned int count,
			  unsigned int flags,
			  unsigned long *out)
{
	struct drm_i915_gem_exec_object2 batch = {};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&batch),
		.buffer_count = 1,
	};
	unsigned long cycles = 0;
	unsigned int spinner;
	igt_spin_t *a, *b;
	int timeline;
	uint32_t A, B;

	srandom(getpid());

	spinner = IGT_SPIN_POLL_RUN;
	if (flags & B_HOSTILE)
		spinner |= IGT_SPIN_NO_PREEMPTION;

	A = gem_context_create(i915);
	set_load_balancer(i915, A, siblings, count, NULL);
	a = igt_spin_new(i915, A, .flags = spinner);
	igt_spin_end(a);
	gem_sync(i915, a->handle);

	B = gem_context_create(i915);
	set_load_balancer(i915, B, siblings, count, NULL);
	b = igt_spin_new(i915, B, .flags = spinner);
	igt_spin_end(b);
	gem_sync(i915, b->handle);

	timeline = sw_sync_timeline_create();

	igt_until_timeout(2) {
		unsigned int master;
		int fence;

		master = 1;
		if (flags & B_MANY)
			master = rand() % count + 1;

		fence = -1;
		if (flags & B_FENCE)
			fence = sw_sync_timeline_create_fence(timeline,
							      cycles + 1);

		igt_spin_reset(a);
		a->execbuf.flags = master | I915_EXEC_FENCE_OUT;
		if (fence != -1) {
			a->execbuf.rsvd2 = fence;
			a->execbuf.flags |= I915_EXEC_FENCE_IN;
		}
		gem_execbuf_wr(i915, &a->execbuf);

		igt_spin_reset(b);
		b->execbuf.flags = master | I915_EXEC_FENCE_OUT;
		if (fence != -1) {
			b->execbuf.rsvd2 = fence;
			b->execbuf.flags |= I915_EXEC_FENCE_IN;
		}
		gem_execbuf_wr(i915, &b->execbuf);

		if (rand() % 1)
			igt_swap(a, b);

		if (flags & B_DELAY)
			usleep(100);

		batch.handle = create_semaphore_to_spinner(i915, a);
		execbuf.rsvd1 = a->execbuf.rsvd1;
		execbuf.rsvd2 = a->execbuf.rsvd2 >> 32;
		do {
			execbuf.flags = rand() % count + 1;
		} while (execbuf.flags == master);
		execbuf.flags |= I915_EXEC_FENCE_SUBMIT;
		gem_execbuf(i915, &execbuf);
		gem_close(i915, batch.handle);

		batch.handle = create_semaphore_to_spinner(i915, b);
		execbuf.rsvd1 = b->execbuf.rsvd1;
		execbuf.rsvd2 = b->execbuf.rsvd2 >> 32;
		do {
			execbuf.flags = rand() % count + 1;
		} while (execbuf.flags == master);
		execbuf.flags |= I915_EXEC_FENCE_SUBMIT;
		gem_execbuf(i915, &execbuf);
		gem_close(i915, batch.handle);

		if (fence != -1) {
			sw_sync_timeline_inc(timeline, 1);
			close(fence);
		}
		close(a->execbuf.rsvd2 >> 32);
		close(b->execbuf.rsvd2 >> 32);

		gem_sync(i915, a->handle);
		gem_sync(i915, b->handle);

		cycles++;
	}

	close(timeline);

	igt_spin_free(i915, a);
	igt_spin_free(i915, b);

	gem_context_destroy(i915, A);
	gem_context_destroy(i915, B);

	*out = cycles;
}

static uint32_t sync_from(int i915, uint32_t addr, uint32_t target)
{
	uint32_t handle = gem_create(i915, 4096);
	uint32_t *map, *cs;

	cs = map = gem_mmap__device_coherent(i915, handle, 0, 4096, PROT_WRITE);

	/* cancel target spinner */
	*cs++ = MI_STORE_DWORD_IMM;
	*cs++ = target + 64;
	*cs++ = 0;
	*cs++ = 0;

	do {
		*cs++ = MI_NOOP;
	} while (offset_in_page(cs) & 63);

	/* wait for them to cancel us */
	*cs++ = MI_BATCH_BUFFER_START | 1 << 8 | 1;
	*cs++ = addr + 16;
	*cs++ = 0;

	/* self-heal */
	*cs++ = MI_STORE_DWORD_IMM;
	*cs++ = addr + 64;
	*cs++ = 0;
	*cs++ = MI_BATCH_BUFFER_START | 1 << 8 | 1;

	*cs++ = MI_BATCH_BUFFER_END;

	munmap(map, 4096);

	return handle;
}

static uint32_t sync_to(int i915, uint32_t addr, uint32_t target)
{
	uint32_t handle = gem_create(i915, 4096);
	uint32_t *map, *cs;

	cs = map = gem_mmap__device_coherent(i915, handle, 0, 4096, PROT_WRITE);

	do {
		*cs++ = MI_NOOP;
	} while (offset_in_page(cs) & 63);

	/* wait to be cancelled */
	*cs++ = MI_BATCH_BUFFER_START | 1 << 8 | 1;
	*cs++ = addr;
	*cs++ = 0;

	*cs++ = MI_NOOP;

	/* cancel their spin as a compliment */
	*cs++ = MI_STORE_DWORD_IMM;
	*cs++ = target + 64;
	*cs++ = 0;
	*cs++ = 0;

	/* self-heal */
	*cs++ = MI_STORE_DWORD_IMM;
	*cs++ = addr + 64;
	*cs++ = 0;
	*cs++ = MI_BATCH_BUFFER_START | 1 << 8 | 1;

	*cs++ = MI_BATCH_BUFFER_END;

	munmap(map, 4096);

	return handle;
}

static void disable_preparser(int i915, uint32_t ctx)
{
	struct drm_i915_gem_exec_object2 obj = {
		.handle = gem_create(i915, 4096),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.rsvd1 = ctx,
	};
	uint32_t *cs;

	cs = gem_mmap__device_coherent(i915, obj.handle, 0, 4096, PROT_WRITE);

	cs[0] = 0x5 << 23 | 1 << 8 | 0; /* disable preparser magic */
	cs[1] = MI_BATCH_BUFFER_END;
	munmap(cs, 4096);

	gem_execbuf(i915, &execbuf);
	gem_close(i915, obj.handle);
}

static void __bonded_sync(int i915,
			  const struct i915_engine_class_instance *siblings,
			  unsigned int count,
			  unsigned int flags,
			  unsigned long *out)
{
	const uint64_t A = 0 << 12, B = 1 << 12;
	struct drm_i915_gem_exec_object2 obj[2] = { {
		.handle = sync_to(i915, A, B),
		.offset = A,
		.flags = EXEC_OBJECT_PINNED
	}, {
		.handle = sync_from(i915, B, A),
		.offset = B,
		.flags = EXEC_OBJECT_PINNED
	} };
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(obj),
		.buffer_count = 2,
		.rsvd1 = gem_context_create(i915),
	};

	unsigned long cycles = 0;
	int timeline = sw_sync_timeline_create();

	if (!(flags & B_HOSTILE)) /* always non-preemptible */
		goto out;

	set_load_balancer(i915, execbuf.rsvd1, siblings, count, NULL);
	disable_preparser(i915, execbuf.rsvd1);

	srandom(getpid());
	igt_until_timeout(2) {
		int master;
		int fence;

		master = 1;
		if (flags & B_MANY)
			master = rand() % count + 1;

		fence = -1;
		if (flags & B_FENCE)
			fence = sw_sync_timeline_create_fence(timeline,
							      cycles + 1);

		execbuf.flags = master | I915_EXEC_FENCE_OUT;
		if (fence != -1) {
			execbuf.rsvd2 = fence;
			execbuf.flags |= I915_EXEC_FENCE_IN;
		}
		gem_execbuf_wr(i915, &execbuf);

		execbuf.rsvd2 >>= 32;
		if (flags & B_DELAY)
			usleep(100);

		igt_swap(obj[0], obj[1]);

		do {
			execbuf.flags = rand() % count + 1;
		} while (execbuf.flags == master);
		execbuf.flags |= I915_EXEC_FENCE_OUT | I915_EXEC_FENCE_SUBMIT;
		gem_execbuf_wr(i915, &execbuf);

		if (fence != -1) {
			sw_sync_timeline_inc(timeline, 1);
			close(fence);
		}

		gem_sync(i915, obj[1].handle);
		gem_sync(i915, obj[0].handle);

		igt_assert_eq(sync_fence_status(execbuf.rsvd2 & 0xffffffff), 1);
		igt_assert_eq(sync_fence_status(execbuf.rsvd2 >> 32), 1);

		close(execbuf.rsvd2);
		close(execbuf.rsvd2 >> 32);

		cycles++;
	}

out:
	close(timeline);
	gem_close(i915, obj[0].handle);
	gem_close(i915, obj[1].handle);
	gem_context_destroy(i915, execbuf.rsvd1);

	*out = cycles;
}

static void
bonded_runner(int i915,
	      void (*fn)(int i915,
			 const struct i915_engine_class_instance *siblings,
			 unsigned int count,
			 unsigned int flags,
			 unsigned long *out))
{
	static const unsigned int phases[] = {
		0,
		B_FENCE,
		B_MANY,
		B_MANY | B_DELAY,
		B_HOSTILE,
		B_HOSTILE | B_FENCE,
		B_HOSTILE | B_DELAY,
	};
	unsigned long *cycles;

	/*
	 * The purpose of bonded submission is to execute one or more requests
	 * concurrently. However, the very nature of that requires coordinated
	 * submission across multiple engines.
	 */
	igt_require(gem_scheduler_has_preemption(i915));

	cycles = mmap(0, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);

	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *siblings;
		unsigned int count;

		siblings = list_engines(i915, 1u << class, &count);
		if (count > 1) {
			igt_info("Class %u, 1 thread\n", class);
			for (int i = 0; i < ARRAY_SIZE(phases); i++) {
				cycles[0] = 0;
				fn(i915, siblings, count, phases[i], &cycles[0]);
				gem_quiescent_gpu(i915);
				if (cycles[0] == 0)
					continue;

				igt_info("%s %s %s submission, %lu cycles\n",
					 phases[i] & B_HOSTILE ? "Non-preemptible" : "Preemptible",
					 phases[i] & B_MANY ? "many-master" : "single-master",
					 phases[i] & B_FENCE ? "fenced" :
					 phases[i] & B_DELAY ? "delayed" :
					 "immediate",
					 cycles[0]);
			}

			igt_info("Class %u, %d threads\n", class, count + 1);
			for (int i = 0; i < ARRAY_SIZE(phases); i++) {
				memset(cycles, 0, (count + 1) * sizeof(*cycles));
				igt_fork(child, count + 1)
					fn(i915,
					   siblings, count,
					   phases[i],
					   &cycles[child]);
				igt_waitchildren();
				gem_quiescent_gpu(i915);

				for (int child = 1; child < count + 1; child++)
					cycles[0] += cycles[child];
				if (cycles[0] == 0)
					continue;

				igt_info("%s %s %s submission, %lu cycles\n",
					 phases[i] & B_HOSTILE ? "Non-preemptible" : "Preemptible",
					 phases[i] & B_MANY ? "many-master" : "single-master",
					 phases[i] & B_FENCE ? "fenced" :
					 phases[i] & B_DELAY ? "delayed" :
					 "immediate",
					 cycles[0]);
			}
		}

		free(siblings);
	}

	munmap(cycles, 4096);
}

static void __bonded_nohang(int i915, uint32_t ctx,
			    const struct i915_engine_class_instance *siblings,
			    unsigned int count,
			    unsigned int flags)
#define NOHANG 0x1
{
	struct drm_i915_gem_exec_object2 batch = {
		.handle = batch_create(i915),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&batch),
		.buffer_count = 1,
		.rsvd1 = ctx,
	};
	igt_spin_t *time, *spin;
	uint32_t load;

	load = gem_context_create(i915);
	gem_context_set_priority(i915, load, 1023);
	set_load_balancer(i915, load, siblings, count, NULL);

	spin = igt_spin_new(i915, load, .engine = 1);

	/* Master on engine 1, stuck behind a spinner */
	execbuf.flags = 1 | I915_EXEC_FENCE_OUT;
	gem_execbuf_wr(i915, &execbuf);

	/* Bond on engine 2, engine clear bond can be submitted immediately */
	execbuf.rsvd2 >>= 32;
	execbuf.flags = 2 | I915_EXEC_FENCE_SUBMIT | I915_EXEC_FENCE_OUT;
	gem_execbuf_wr(i915, &execbuf);

	igt_debugfs_dump(i915, "i915_engine_info");

	/* The master will remain blocked until the spinner is reset */
	time = igt_spin_new(i915, .flags = IGT_SPIN_NO_PREEMPTION); /* rcs0 */
	while (gem_bo_busy(i915, time->handle)) {
		igt_spin_t *next;

		if (flags & NOHANG) {
			/* Keep replacing spin, so that it doesn't hang */
			next = igt_spin_new(i915, load, .engine = 1);
			igt_spin_free(i915, spin);
			spin = next;
		}

		if (!gem_bo_busy(i915, batch.handle))
			break;
	}
	igt_spin_free(i915, time);
	igt_spin_free(i915, spin);

	/* Check the bonded pair completed and were not declared hung */
	igt_assert_eq(sync_fence_status(execbuf.rsvd2 & 0xffffffff), 1);
	igt_assert_eq(sync_fence_status(execbuf.rsvd2 >> 32), 1);

	close(execbuf.rsvd2);
	close(execbuf.rsvd2 >> 32);

	gem_context_destroy(i915, load);
	gem_close(i915, batch.handle);
}

static void bonded_nohang(int i915, unsigned int flags)
{
	uint32_t ctx;

	/*
	 * We try and trick ourselves into declaring a bonded request as
	 * hung by preventing the master from running [after submission].
	 */

	igt_require(gem_scheduler_has_semaphores(i915));

	ctx = gem_context_create(i915);

	for (int class = 1; class < 32; class++) {
		struct i915_engine_class_instance *siblings;
		unsigned int count;

		siblings = list_engines(i915, 1u << class, &count);
		if (count > 1)
			__bonded_nohang(i915, ctx, siblings, count, flags);
		free(siblings);
	}

	gem_context_destroy(i915, ctx);
}

static void indices(int i915)
{
	I915_DEFINE_CONTEXT_PARAM_ENGINES(engines, I915_EXEC_RING_MASK + 1);
	struct drm_i915_gem_context_param p = {
		.ctx_id = gem_context_create(i915),
		.param = I915_CONTEXT_PARAM_ENGINES,
		.value = to_user_pointer(&engines)
	};

	struct drm_i915_gem_exec_object2 batch = {
		.handle = batch_create(i915),
	};

	unsigned int nengines = 0;
	void *balancers = NULL;

	/*
	 * We can populate our engine map with multiple virtual engines.
	 * Do so.
	 */

	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *ci;
		unsigned int count;

		ci = list_engines(i915, 1u << class, &count);
		if (!ci)
			continue;

		for (int n = 0; n < count; n++) {
			struct i915_context_engines_load_balance *balancer;

			engines.engines[nengines].engine_class =
				I915_ENGINE_CLASS_INVALID;
			engines.engines[nengines].engine_instance =
				I915_ENGINE_CLASS_INVALID_NONE;

			balancer = calloc(sizeof_load_balance(count), 1);
			igt_assert(balancer);

			balancer->base.name =
				I915_CONTEXT_ENGINES_EXT_LOAD_BALANCE;
			balancer->base.next_extension =
				to_user_pointer(balancers);
			balancers = balancer;

			balancer->engine_index = nengines++;
			balancer->num_siblings = count;

			memcpy(balancer->engines,
			       ci, count * sizeof(*ci));
		}
		free(ci);
	}

	igt_require(balancers);
	engines.extensions = to_user_pointer(balancers);
	p.size = (sizeof(struct i915_engine_class_instance) * nengines +
		  sizeof(struct i915_context_param_engines));
	gem_context_set_param(i915, &p);

	for (unsigned int n = 0; n < nengines; n++) {
		struct drm_i915_gem_execbuffer2 eb = {
			.buffers_ptr = to_user_pointer(&batch),
			.buffer_count = 1,
			.flags = n,
			.rsvd1 = p.ctx_id,
		};
		igt_debug("Executing on index=%d\n", n);
		gem_execbuf(i915, &eb);
	}
	gem_context_destroy(i915, p.ctx_id);

	gem_sync(i915, batch.handle);
	gem_close(i915, batch.handle);

	while (balancers) {
		struct i915_context_engines_load_balance *b, *n;

		b = balancers;
		n = from_user_pointer(b->base.next_extension);
		free(b);

		balancers = n;
	}

	gem_quiescent_gpu(i915);
}

static void __bonded_early(int i915,
			   const struct i915_engine_class_instance *siblings,
			   unsigned int count,
			   unsigned int flags)
{
	I915_DEFINE_CONTEXT_ENGINES_BOND(bonds[count], 1);
	uint32_t handle = batch_create(i915);
	struct drm_i915_gem_exec_object2 batch = {
		.handle = handle,
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&batch),
		.buffer_count = 1,
	};
	uint32_t vm, ctx;
	igt_spin_t *spin;

	memset(bonds, 0, sizeof(bonds));
	for (int n = 0; n < ARRAY_SIZE(bonds); n++) {
		bonds[n].base.name = I915_CONTEXT_ENGINES_EXT_BOND;
		bonds[n].base.next_extension =
			n ? to_user_pointer(&bonds[n - 1]) : 0;

		bonds[n].master = siblings[n];
		bonds[n].num_bonds = 1;
		bonds[n].engines[0] = siblings[(n + 1) % count];
	}

	/* We share a VM so that the spin cancel will work without a reloc */
	vm = gem_vm_create(i915);

	ctx = gem_context_create(i915);
	set_vm(i915, ctx, vm);
	set_load_balancer(i915, ctx, siblings, count,
			  flags & VIRTUAL_ENGINE ? &bonds : NULL);

	/* A: spin forever on engine 1 */
	spin = igt_spin_new(i915,
			    .ctx_id = ctx,
			    .engine = (flags & VIRTUAL_ENGINE) ? 0 : 1,
			    .flags = IGT_SPIN_NO_PREEMPTION);

	/* B: runs after A on engine 1 */
	execbuf.rsvd1 = ctx;
	execbuf.flags = I915_EXEC_FENCE_OUT;
	execbuf.flags |= spin->execbuf.flags & 63;
	gem_execbuf_wr(i915, &execbuf);

	/* B': run in parallel with B on engine 2, i.e. not before A! */
	execbuf.flags = I915_EXEC_FENCE_SUBMIT | I915_EXEC_FENCE_OUT;
	if(!(flags & VIRTUAL_ENGINE))
		execbuf.flags |= 2;
	execbuf.rsvd2 >>= 32;
	gem_execbuf_wr(i915, &execbuf);

	/* C: prevent anything running on engine 2 after B' */
	spin->execbuf.flags = execbuf.flags & 63;
	gem_execbuf(i915, &spin->execbuf);

	igt_debugfs_dump(i915, "i915_engine_info");

	/* D: cancel the spinner from engine 2 (new context) */
	gem_context_destroy(i915, ctx);
	ctx = gem_context_create(i915);
	set_vm(i915, ctx, vm);
	set_load_balancer(i915, ctx, siblings, count,
			  flags & VIRTUAL_ENGINE ? &bonds : NULL);
	batch.handle = create_semaphore_to_spinner(i915, spin);
	execbuf.rsvd1 = ctx;
	execbuf.flags = 0;
	if(!(flags & VIRTUAL_ENGINE))
		execbuf.flags |= 2;
	gem_execbuf(i915, &execbuf);
	gem_close(i915, batch.handle);

	/* If C runs before D, we never cancel the spinner and so hang */
	gem_sync(i915, handle);

	/* Check the bonded pair completed successfully */
	igt_assert_eq(sync_fence_status(execbuf.rsvd2 & 0xffffffff), 1);
	igt_assert_eq(sync_fence_status(execbuf.rsvd2 >> 32), 1);

	close(execbuf.rsvd2);
	close(execbuf.rsvd2 >> 32);

	gem_context_destroy(i915, ctx);
	gem_close(i915, handle);
	igt_spin_free(i915, spin);
}

static void bonded_early(int i915)
{
	/*
	 * Our goal is to start the bonded payloads at roughly the same time.
	 * We do not want to start the secondary batch too early as it will
	 * do nothing but hog the GPU until the first has a chance to execute.
	 * So if we were to arbitrary delay the first by running it after a
	 * spinner...
	 *
	 * By using a pair of spinners, we can create a bonded hog that when
	 * set in motion will fully utilize both engines [if the scheduling is
	 * incorrect]. We then use a third party submitted after the bonded
	 * pair to cancel the spinner from the GPU -- if it is unable to run,
	 * the spinner is never cancelled, and the bonded pair will cause a GPU
	 * hang.
	 */

	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *siblings;
		unsigned int count;

		siblings = list_engines(i915, 1u << class, &count);
		if (count > 1) {
			__bonded_early(i915, siblings, count, 0);
			__bonded_early(i915, siblings, count, VIRTUAL_ENGINE);
		}
		free(siblings);
	}
}

static void busy(int i915)
{
	uint32_t scratch = gem_create(i915, 4096);

	/*
	 * Check that virtual engines are reported via GEM_BUSY.
	 *
	 * When running, the batch will be on the real engine and report
	 * the actual class.
	 *
	 * Prior to running, if the load-balancer is across multiple
	 * classes we don't know which engine the batch will
	 * execute on, so we report them all!
	 *
	 * However, as we only support (and test) creating a load-balancer
	 * from engines of only one class, that can be propagated accurately
	 * through to GEM_BUSY.
	 */

	for (int class = 0; class < 16; class++) {
		struct drm_i915_gem_busy busy;
		struct i915_engine_class_instance *ci;
		unsigned int count;
		igt_spin_t *spin[2];
		uint32_t ctx;

		ci = list_engines(i915, 1u << class, &count);
		if (!ci)
			continue;

		ctx = load_balancer_create(i915, ci, count);
		free(ci);

		spin[0] = __igt_spin_new(i915,
					 .ctx_id = ctx,
					 .flags = IGT_SPIN_POLL_RUN);
		spin[1] = __igt_spin_new(i915,
					 .ctx_id = ctx,
					 .dependency = scratch);

		igt_spin_busywait_until_started(spin[0]);

		/* Running: actual class */
		busy.handle = spin[0]->handle;
		do_ioctl(i915, DRM_IOCTL_I915_GEM_BUSY, &busy);
		igt_assert_eq_u32(busy.busy, 1u << (class + 16));

		/* Queued(read, maybe write if being migrated): expected class */
		busy.handle = spin[1]->handle;
		do_ioctl(i915, DRM_IOCTL_I915_GEM_BUSY, &busy);
		igt_assert_eq_u32(busy.busy & 0xffff << 16, 1u << (class + 16));

		/* Queued(write): expected class */
		busy.handle = scratch;
		do_ioctl(i915, DRM_IOCTL_I915_GEM_BUSY, &busy);
		igt_assert_eq_u32(busy.busy,
				  (1u << (class + 16)) | (class + 1));

		igt_spin_free(i915, spin[1]);
		igt_spin_free(i915, spin[0]);

		gem_context_destroy(i915, ctx);
	}

	gem_close(i915, scratch);
	gem_quiescent_gpu(i915);
}

static void full(int i915, unsigned int flags)
#define PULSE 0x1
#define LATE 0x2
{
	struct drm_i915_gem_exec_object2 batch = {
		.handle = batch_create(i915),
	};

	if (flags & LATE)
		igt_require_sw_sync();

	/*
	 * I915_CONTEXT_PARAM_ENGINE changes the meaning of engine selector in
	 * execbuf to utilize our own map, into which we replace I915_EXEC_DEFAULT
	 * to provide an automatic selection from the other ctx->engine[]. It
	 * employs load-balancing to evenly distribute the workload the
	 * array. If we submit N spinners, we expect them to be simultaneously
	 * running across N engines and use PMU to confirm that the entire
	 * set of engines are busy.
	 *
	 * We complicate matters by interspersing short-lived tasks to
	 * challenge the kernel to search for space in which to insert new
	 * batches.
	 */

	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *ci;
		igt_spin_t *spin = NULL;
		IGT_CORK_FENCE(cork);
		unsigned int count;
		double load;
		int fence = -1;
		int *pmu;

		ci = list_engines(i915, 1u << class, &count);
		if (!ci)
			continue;

		pmu = malloc(sizeof(*pmu) * count);
		igt_assert(pmu);

		if (flags & LATE)
			fence = igt_cork_plug(&cork, i915);

		pmu[0] = -1;
		for (unsigned int n = 0; n < count; n++) {
			uint32_t ctx;

			pmu[n] = add_pmu(i915, pmu[0], &ci[n]);

			if (flags & PULSE) {
				struct drm_i915_gem_execbuffer2 eb = {
					.buffers_ptr = to_user_pointer(&batch),
					.buffer_count = 1,
					.rsvd2 = fence,
					.flags = flags & LATE ? I915_EXEC_FENCE_IN : 0,
				};
				gem_execbuf(i915, &eb);
			}

			/*
			 * Each spinner needs to be one a new timeline,
			 * otherwise they will just sit in the single queue
			 * and not run concurrently.
			 */
			ctx = load_balancer_create(i915, ci, count);

			if (spin == NULL) {
				spin = __igt_spin_new(i915, .ctx_id = ctx);
			} else {
				struct drm_i915_gem_execbuffer2 eb = {
					.buffers_ptr = spin->execbuf.buffers_ptr,
					.buffer_count = spin->execbuf.buffer_count,
					.rsvd1 = ctx,
					.rsvd2 = fence,
					.flags = flags & LATE ? I915_EXEC_FENCE_IN : 0,
				};
				gem_execbuf(i915, &eb);
			}

			gem_context_destroy(i915, ctx);
		}

		if (flags & LATE) {
			igt_cork_unplug(&cork);
			close(fence);
		}

		load = measure_min_load(pmu[0], count, 10000);
		igt_spin_free(i915, spin);

		close(pmu[0]);
		free(pmu);

		free(ci);

		igt_assert_f(load > 0.90,
			     "minimum load for %d x class:%d was found to be only %.1f%% busy\n",
			     count, class, load*100);
		gem_quiescent_gpu(i915);
	}

	gem_close(i915, batch.handle);
	gem_quiescent_gpu(i915);
}

static void __sliced(int i915,
		     uint32_t ctx, unsigned int count,
		     unsigned int flags)
{
	igt_spin_t *load[count];
	igt_spin_t *virtual;

	virtual = igt_spin_new(i915, ctx, .engine = 0,
			       .flags = (IGT_SPIN_FENCE_OUT |
					 IGT_SPIN_POLL_RUN));
	for (int i = 0; i < count; i++)
		load[i] = __igt_spin_new(i915, ctx,
					 .engine = i + 1,
					 .fence = virtual->out_fence,
					 .flags = flags);

	/* Wait long enough for the virtual timeslice [1 ms] to expire */
	igt_spin_busywait_until_started(virtual);
	usleep(50 * 1000); /* 50ms */

	igt_spin_end(virtual);
	igt_assert_eq(sync_fence_wait(virtual->out_fence, 1000), 0);
	igt_assert_eq(sync_fence_status(virtual->out_fence), 1);

	for (int i = 0; i < count; i++)
		igt_spin_free(i915, load[i]);
	igt_spin_free(i915, virtual);
}

static void sliced(int i915)
{
	/*
	 * Let's investigate what happens when the virtual request is
	 * timesliced away.
	 *
	 * If the engine is busy with independent work, we want the virtual
	 * request to hop over to an idle engine (within its balancing set).
	 * However, if the work is dependent upon the virtual request,
	 * we most certainly do not want to reschedule that work ahead of
	 * the virtual request. [If we did, we should still have the saving
	 * grace of being able to move the virual request to another engine
	 * and so run both in parallel.] If we do neither, and get stuck
	 * on the dependent work and never run the virtual request, we hang.
	 */

	igt_require(gem_scheduler_has_preemption(i915));
	igt_require(gem_scheduler_has_semaphores(i915));

	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *ci;
		unsigned int count;

		ci = list_engines(i915, 1u << class, &count);
		if (!ci)
			continue;

		if (count < 2) {
			free(ci);
			continue;
		}

		igt_fork(child, count) {
			uint32_t ctx = load_balancer_create(i915, ci, count);

			/* Independent load */
			__sliced(i915, ctx, count, 0);

			/* Dependent load */
			__sliced(i915, ctx, count, IGT_SPIN_FENCE_IN);

			gem_context_destroy(i915, ctx);
		}
		igt_waitchildren();

		free(ci);
	}

	gem_quiescent_gpu(i915);
}

static void __hog(int i915, uint32_t ctx, unsigned int count)
{
	int64_t timeout = 50 * 1000 * 1000; /* 50ms */
	igt_spin_t *virtual;
	igt_spin_t *hog;

	virtual = igt_spin_new(i915, ctx, .engine = 0);
	for (int i = 0; i < count; i++)
		gem_execbuf(i915, &virtual->execbuf);
	usleep(50 * 1000); /* 50ms, long enough to spread across all engines */

	gem_context_set_priority(i915, ctx, 1023);
	hog = __igt_spin_new(i915, ctx,
			     .engine = 1 + (random() % count),
			     .flags = (IGT_SPIN_POLL_RUN |
				       IGT_SPIN_NO_PREEMPTION));
	gem_context_set_priority(i915, ctx, 0);

	/* No matter which engine we choose, we'll have interrupted someone */
	igt_spin_busywait_until_started(hog);

	igt_spin_end(virtual);
	if (gem_wait(i915, virtual->handle, &timeout)) {
		igt_debugfs_dump(i915, "i915_engine_info");
		igt_assert_eq(gem_wait(i915, virtual->handle, &timeout), 0);
	}

	igt_spin_free(i915, hog);
	igt_spin_free(i915, virtual);
}

static void hog(int i915)
{
	/*
	 * Suppose there we are, happily using an engine, minding our
	 * own business, when all of a sudden a very important process
	 * takes over the engine and refuses to let go. Clearly we have
	 * to vacate that engine and find a new home.
	 */

	igt_require(gem_scheduler_has_preemption(i915));
	igt_require(gem_scheduler_has_semaphores(i915));

	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *ci;
		unsigned int count;
		uint32_t ctx;

		ci = list_engines(i915, 1u << class, &count);
		if (!ci)
			continue;

		if (count < 2) {
			free(ci);
			continue;
		}

		ctx = load_balancer_create(i915, ci, count);

		__hog(i915, ctx, count);

		gem_context_destroy(i915, ctx);
		igt_waitchildren();

		free(ci);
	}

	gem_quiescent_gpu(i915);
}

static uint32_t sema_create(int i915, uint64_t addr, uint32_t **x)
{
	uint32_t handle = gem_create(i915, 4096);

	*x = gem_mmap__device_coherent(i915, handle, 0, 4096, PROT_WRITE);
	for (int n = 1; n <= 32; n++) {
		uint32_t *cs = *x + n * 16;

		*cs++ = MI_SEMAPHORE_WAIT |
			MI_SEMAPHORE_POLL |
			MI_SEMAPHORE_SAD_GTE_SDD |
			(4 - 2);
		*cs++ = n;
		*cs++ = addr;
		*cs++ = addr >> 32;

		*cs++ = MI_BATCH_BUFFER_END;
	}

	return handle;
}

static int __execbuf(int i915, struct drm_i915_gem_execbuffer2 *execbuf)
{
	int err;

	err = 0;
	if (ioctl(i915, DRM_IOCTL_I915_GEM_EXECBUFFER2, execbuf)) {
		err = -errno;
		igt_assume(err);
	}

	errno = 0;
	return err;
}

static uint32_t *sema(int i915, struct i915_engine_class_instance *ci,
		      unsigned int count)
{
	uint32_t *ctl;
	struct drm_i915_gem_exec_object2 batch = {
		.handle = sema_create(i915, 64 << 20, &ctl),
		.offset = 64 << 20,
		.flags = EXEC_OBJECT_PINNED
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&batch),
		.buffer_count = 1,
		.rsvd1 = load_balancer_create(i915, ci, count),
	};

	for (int n = 1; n <= 32; n++) {
		int64_t poll = 1;

		execbuf.batch_start_offset = 64 * n;
		if (__execbuf(i915, &execbuf))
			break;

		/* Force a breadcrumb to be installed on each request */
		gem_wait(i915, batch.handle, &poll);
	}

	gem_context_destroy(i915, execbuf.rsvd1);

	igt_assert(gem_bo_busy(i915, batch.handle));
	gem_close(i915, batch.handle);

	return ctl;
}

static void __waits(int i915, int timeout,
		    struct i915_engine_class_instance *ci,
		    unsigned int count)
{
	uint32_t *semaphores[count + 1];

	for (int i = 0; i <= count; i++)
		semaphores[i] = sema(i915, ci, count);

	igt_until_timeout(timeout) {
		int i = rand() % (count + 1);

		/* Let the occasional timeslice pass naturally */
		usleep(rand() % 2000);

		/* Complete a variable number of requests in each pass */
		if ((*semaphores[i] += rand() % 32) >= 32) {
			*semaphores[i] = 0xffffffff;
			munmap(semaphores[i], 4096);
			semaphores[i] = sema(i915, ci, count);
		}
	}

	for (int i = 0; i <= count; i++) {
		*semaphores[i] = 0xffffffff;
		munmap(semaphores[i], 4096);
	}
}

static void waits(int i915, int timeout)
{
	bool nonblock;

	nonblock = fcntl(i915, F_GETFL) & O_NONBLOCK;
	if (!nonblock)
		fcntl(i915, F_SETFL, fcntl(i915, F_GETFL) | O_NONBLOCK);

	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *ci;
		unsigned int count;

		ci = list_engines(i915, 1u << class, &count);
		if (!ci)
			continue;

		if (count > 1) {
			uint32_t ctx = load_balancer_create(i915, ci, count);

			__waits(i915, timeout, ci, count);

			gem_context_destroy(i915, ctx);
		}

		free(ci);
	}

	if (!nonblock)
		fcntl(i915, F_SETFL, fcntl(i915, F_GETFL) & ~O_NONBLOCK);

	gem_quiescent_gpu(i915);
}

static void nop(int i915)
{
	struct drm_i915_gem_exec_object2 batch = {
		.handle = batch_create(i915),
	};

	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *ci;
		unsigned int count;
		uint32_t ctx;

		ci = list_engines(i915, 1u << class, &count);
		if (!ci)
			continue;

		ctx = load_balancer_create(i915, ci, count);

		for (int n = 0; n < count; n++) {
			struct drm_i915_gem_execbuffer2 execbuf = {
				.buffers_ptr = to_user_pointer(&batch),
				.buffer_count = 1,
				.flags = n + 1,
				.rsvd1 = ctx,
			};
			struct timespec tv = {};
			unsigned long nops;
			double t;

			igt_nsec_elapsed(&tv);
			nops = 0;
			do {
				for (int r = 0; r < 1024; r++)
					gem_execbuf(i915, &execbuf);
				nops += 1024;
			} while (igt_seconds_elapsed(&tv) < 2);
			gem_sync(i915, batch.handle);

			t = igt_nsec_elapsed(&tv) * 1e-3 / nops;
			igt_info("%s:%d %.3fus\n", class_to_str(class), n, t);
		}

		{
			struct drm_i915_gem_execbuffer2 execbuf = {
				.buffers_ptr = to_user_pointer(&batch),
				.buffer_count = 1,
				.rsvd1 = ctx,
			};
			struct timespec tv = {};
			unsigned long nops;
			double t;

			igt_nsec_elapsed(&tv);
			nops = 0;
			do {
				for (int r = 0; r < 1024; r++)
					gem_execbuf(i915, &execbuf);
				nops += 1024;
			} while (igt_seconds_elapsed(&tv) < 2);
			gem_sync(i915, batch.handle);

			t = igt_nsec_elapsed(&tv) * 1e-3 / nops;
			igt_info("%s:* %.3fus\n", class_to_str(class), t);
		}


		igt_fork(child, count) {
			struct drm_i915_gem_execbuffer2 execbuf = {
				.buffers_ptr = to_user_pointer(&batch),
				.buffer_count = 1,
				.flags = child + 1,
				.rsvd1 = load_balancer_create(i915, ci, count),
			};
			struct timespec tv = {};
			unsigned long nops;
			double t;

			igt_nsec_elapsed(&tv);
			nops = 0;
			do {
				for (int r = 0; r < 1024; r++)
					gem_execbuf(i915, &execbuf);
				nops += 1024;
			} while (igt_seconds_elapsed(&tv) < 2);
			gem_sync(i915, batch.handle);

			t = igt_nsec_elapsed(&tv) * 1e-3 / nops;
			igt_info("[%d] %s:%d %.3fus\n",
				 child, class_to_str(class), child, t);

			memset(&tv, 0, sizeof(tv));
			execbuf.flags = 0;

			igt_nsec_elapsed(&tv);
			nops = 0;
			do {
				for (int r = 0; r < 1024; r++)
					gem_execbuf(i915, &execbuf);
				nops += 1024;
			} while (igt_seconds_elapsed(&tv) < 2);
			gem_sync(i915, batch.handle);

			t = igt_nsec_elapsed(&tv) * 1e-3 / nops;
			igt_info("[%d] %s:* %.3fus\n",
				 child, class_to_str(class), t);

			gem_context_destroy(i915, execbuf.rsvd1);
		}

		igt_waitchildren();

		gem_context_destroy(i915, ctx);
		free(ci);
	}

	gem_close(i915, batch.handle);
	gem_quiescent_gpu(i915);
}

static void sequential(int i915)
{
	struct drm_i915_gem_exec_object2 batch = {
		.handle = batch_create(i915),
	};

	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *ci;
		struct drm_i915_gem_execbuffer2 execbuf = {
			.buffers_ptr = to_user_pointer(&batch),
			.buffer_count = 1,
			.flags = I915_EXEC_FENCE_OUT,
		};
		struct timespec tv = {};
		unsigned int count;
		unsigned long nops;
		double t;
		uint32_t *ctx;

		ci = list_engines(i915, 1u << class, &count);
		if (!ci || count < 2)
			goto next;

		ctx = malloc(sizeof(*ctx) * count);
		for (int n = 0; n < count; n++)
			ctx[n] = load_balancer_create(i915, ci, count);

		gem_execbuf_wr(i915, &execbuf);
		execbuf.rsvd2 >>= 32;
		execbuf.flags |= I915_EXEC_FENCE_IN;
		gem_sync(i915, batch.handle);

		nops = 0;
		igt_nsec_elapsed(&tv);
		do {
			for (int n = 0; n < count; n++) {
				execbuf.rsvd1 = ctx[n];
				gem_execbuf_wr(i915, &execbuf);
				close(execbuf.rsvd2);
				execbuf.rsvd2 >>= 32;
			}
			nops += count;
		} while (igt_seconds_elapsed(&tv) < 2);
		gem_sync(i915, batch.handle);

		t = igt_nsec_elapsed(&tv) * 1e-3 / nops;
		igt_info("%s: %.3fus\n", class_to_str(class), t);

		close(execbuf.rsvd2);
		for (int n = 0; n < count; n++)
			gem_context_destroy(i915, ctx[n]);
		free(ctx);
next:
		free(ci);
	}

	gem_close(i915, batch.handle);
	gem_quiescent_gpu(i915);
}

static void ping(int i915, uint32_t ctx, unsigned int engine)
{
	struct drm_i915_gem_exec_object2 obj = {
		.handle = batch_create(i915),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = engine,
		.rsvd1 = ctx,
	};
	gem_execbuf(i915, &execbuf);
	gem_sync(i915, obj.handle);
	gem_close(i915, obj.handle);
}

static void semaphore(int i915)
{
	uint32_t scratch;
	igt_spin_t *spin[3];

	/*
	 * If we are using HW semaphores to launch serialised requests
	 * on different engine concurrently, we want to verify that real
	 * work is unimpeded.
	 */
	igt_require(gem_scheduler_has_preemption(i915));

	scratch = gem_create(i915, 4096);
	spin[2] = igt_spin_new(i915, .dependency = scratch);
	for (int class = 1; class < 32; class++) {
		struct i915_engine_class_instance *ci;
		unsigned int count;
		uint32_t block[2], vip;

		ci = list_engines(i915, 1u << class, &count);
		if (!ci)
			continue;

		if (count < ARRAY_SIZE(block))
			continue;

		/* Ensure that we completely occupy all engines in this group */
		count = ARRAY_SIZE(block);

		for (int i = 0; i < count; i++) {
			block[i] = gem_context_create(i915);
			set_load_balancer(i915, block[i], ci, count, NULL);
			spin[i] = __igt_spin_new(i915,
						 .ctx_id = block[i],
						 .dependency = scratch);
		}

		/*
		 * Either we haven't blocked both engines with semaphores,
		 * or we let the vip through. If not, we hang.
		 */
		vip = gem_context_create(i915);
		set_load_balancer(i915, vip, ci, count, NULL);
		ping(i915, vip, 0);
		gem_context_destroy(i915, vip);

		for (int i = 0; i < count; i++) {
			igt_spin_free(i915, spin[i]);
			gem_context_destroy(i915, block[i]);
		}

		free(ci);
	}
	igt_spin_free(i915, spin[2]);
	gem_close(i915, scratch);

	gem_quiescent_gpu(i915);
}

static void set_unbannable(int i915, uint32_t ctx)
{
	struct drm_i915_gem_context_param p = {
		.ctx_id = ctx,
		.param = I915_CONTEXT_PARAM_BANNABLE,
	};

	igt_assert_eq(__gem_context_set_param(i915, &p), 0);
}

static void hangme(int i915)
{
	struct drm_i915_gem_exec_object2 batch = {
		.handle = batch_create(i915),
	};

	/*
	 * Fill the available engines with hanging virtual engines and verify
	 * that execution continues onto the second batch.
	 */

	for (int class = 1; class < 32; class++) {
		struct i915_engine_class_instance *ci;
		IGT_CORK_FENCE(cork);
		struct client {
			igt_spin_t *spin[2];
		} *client;
		unsigned int count;
		uint32_t bg;
		int fence;

		ci = list_engines(i915, 1u << class, &count);
		if (!ci)
			continue;

		if (count < 2) {
			free(ci);
			continue;
		}

		client = malloc(sizeof(*client) * count);
		igt_assert(client);

		fence = igt_cork_plug(&cork, i915);
		for (int i = 0; i < count; i++) {
			uint32_t ctx = gem_context_create(i915);
			struct client *c = &client[i];
			unsigned int flags;

			set_unbannable(i915, ctx);
			set_load_balancer(i915, ctx, ci, count, NULL);

			flags = IGT_SPIN_FENCE_IN |
				IGT_SPIN_FENCE_OUT |
				IGT_SPIN_NO_PREEMPTION;
			if (!gem_has_cmdparser(i915, ALL_ENGINES))
				flags |= IGT_SPIN_INVALID_CS;
			for (int j = 0; j < ARRAY_SIZE(c->spin); j++)  {
				c->spin[j] = __igt_spin_new(i915, ctx,
							    .fence = fence,
							    .flags = flags);
				flags = IGT_SPIN_FENCE_OUT;
			}

			gem_context_destroy(i915, ctx);
		}
		close(fence);
		igt_cork_unplug(&cork); /* queue all hangs en masse */

		/* Apply some background context to speed up hang detection */
		bg = gem_context_create(i915);
		set_engines(i915, bg, ci, count);
		gem_context_set_priority(i915, bg, 1023);
		for (int i = 0; i < count; i++) {
			struct drm_i915_gem_execbuffer2 execbuf = {
				.buffers_ptr = to_user_pointer(&batch),
				.buffer_count = 1,
				.flags = i,
				.rsvd1 = bg,
			};
			gem_execbuf(i915, &execbuf);
		}
		gem_context_destroy(i915, bg);

		for (int i = 0; i < count; i++) {
			struct client *c = &client[i];
			int64_t timeout;

			igt_debug("Waiting for client[%d].spin[%d]\n", i, 0);
			timeout = NSEC_PER_SEC / 2;
			if (gem_wait(i915, c->spin[0]->handle, &timeout))
				igt_debugfs_dump(i915, "i915_engine_info");
			gem_sync(i915, c->spin[0]->handle);
			igt_assert_eq(sync_fence_status(c->spin[0]->out_fence),
				      -EIO);

			igt_debug("Waiting for client[%d].spin[%d]\n", i, 1);
			timeout = NSEC_PER_SEC / 2;
			if (gem_wait(i915, c->spin[1]->handle, &timeout))
				igt_debugfs_dump(i915, "i915_engine_info");
			igt_assert_eq(sync_fence_status(c->spin[1]->out_fence),
				      -EIO);

			igt_spin_free(i915, c->spin[0]);
			igt_spin_free(i915, c->spin[1]);
		}
		free(client);
	}

	gem_close(i915, batch.handle);
	gem_quiescent_gpu(i915);
}

static void smoketest(int i915, int timeout)
{
	struct drm_i915_gem_exec_object2 batch[2] = {
		{ .handle = __batch_create(i915, 16380) }
	};
	unsigned int ncontext = 0;
	uint32_t *contexts = NULL;
	uint32_t *handles = NULL;

	igt_require_sw_sync();

	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *ci;
		unsigned int count = 0;

		ci = list_engines(i915, 1u << class, &count);
		if (!ci || count < 2) {
			free(ci);
			continue;
		}

		ncontext += 128;
		contexts = realloc(contexts, sizeof(*contexts) * ncontext);
		igt_assert(contexts);

		for (unsigned int n = ncontext - 128; n < ncontext; n++) {
			contexts[n] = load_balancer_create(i915, ci, count);
			igt_assert(contexts[n]);
		}

		free(ci);
	}
	if (!ncontext) /* suppress the fluctuating status of shard-icl */
		return;

	igt_debug("Created %d virtual engines (one per context)\n", ncontext);
	contexts = realloc(contexts, sizeof(*contexts) * ncontext * 4);
	igt_assert(contexts);
	memcpy(contexts + ncontext, contexts, ncontext * sizeof(*contexts));
	ncontext *= 2;
	memcpy(contexts + ncontext, contexts, ncontext * sizeof(*contexts));
	ncontext *= 2;

	handles = malloc(sizeof(*handles) * ncontext);
	igt_assert(handles);
	for (unsigned int n = 0; n < ncontext; n++)
		handles[n] = gem_create(i915, 4096);

	igt_until_timeout(timeout) {
		unsigned int count = 1 + (rand() % (ncontext - 1));
		IGT_CORK_FENCE(cork);
		int fence = igt_cork_plug(&cork, i915);

		for (unsigned int n = 0; n < count; n++) {
			struct drm_i915_gem_execbuffer2 eb = {
				.buffers_ptr = to_user_pointer(batch),
				.buffer_count = ARRAY_SIZE(batch),
				.rsvd1 = contexts[n],
				.rsvd2 = fence,
				.flags = I915_EXEC_BATCH_FIRST | I915_EXEC_FENCE_IN,
			};
			batch[1].handle = handles[n];
			gem_execbuf(i915, &eb);
		}
		igt_permute_array(handles, count, igt_exchange_int);

		igt_cork_unplug(&cork);
		for (unsigned int n = 0; n < count; n++)
			gem_sync(i915, handles[n]);

		close(fence);
	}

	for (unsigned int n = 0; n < ncontext; n++) {
		gem_close(i915, handles[n]);
		__gem_context_destroy(i915, contexts[n]);
	}
	free(handles);
	free(contexts);
	gem_close(i915, batch[0].handle);
}

static uint32_t read_ctx_timestamp(int i915, uint32_t ctx)
{
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_exec_object2 obj = {
		.handle = gem_create(i915, 4096),
		.offset = 32 << 20,
		.relocs_ptr = to_user_pointer(&reloc),
		.relocation_count = 1,
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.rsvd1 = ctx,
	};
	uint32_t *map, *cs;
	uint32_t ts;

	cs = map = gem_mmap__device_coherent(i915, obj.handle,
					     0, 4096, PROT_WRITE);

	*cs++ = 0x24 << 23 | 1 << 19 | 2; /* relative SRM */
	*cs++ = 0x3a8; /* CTX_TIMESTAMP */
	memset(&reloc, 0, sizeof(reloc));
	reloc.target_handle = obj.handle;
	reloc.presumed_offset = obj.offset;
	reloc.offset = offset_in_page(cs);
	reloc.delta = 4000;
	*cs++ = obj.offset + 4000;
	*cs++ = obj.offset >> 32;

	*cs++ = MI_BATCH_BUFFER_END;

	gem_execbuf(i915, &execbuf);
	gem_sync(i915, obj.handle);
	gem_close(i915, obj.handle);

	ts = map[1000];
	munmap(map, 4096);

	return ts;
}

static int cmp_u32(const void *A, const void *B)
{
	const uint32_t *a = A, *b = B;

	if (*a < *b)
		return -1;
	else if (*a > *b)
		return 1;
	else
		return 0;
}

static int read_ctx_timestamp_frequency(int i915)
{
	int value = 12500000; /* icl!!! are you feeling alright? CTX vs CS */
	drm_i915_getparam_t gp = {
		.value = &value,
		.param = I915_PARAM_CS_TIMESTAMP_FREQUENCY,
	};
	if (intel_gen(intel_get_drm_devid(i915)) != 11)
		ioctl(i915, DRM_IOCTL_I915_GETPARAM, &gp);
	return value;
}

static uint64_t div64_u64_round_up(uint64_t x, uint64_t y)
{
	return (x + y - 1) / y;
}

static uint64_t ticks_to_ns(int i915, uint64_t ticks)
{
	return div64_u64_round_up(ticks * NSEC_PER_SEC,
				  read_ctx_timestamp_frequency(i915));
}

static void __fairslice(int i915,
			const struct i915_engine_class_instance *ci,
			unsigned int count,
			int duration)
{
	const double timeslice_duration_ns = 1e6;
	igt_spin_t *spin = NULL;
	uint32_t ctx[count + 1];
	uint32_t ts[count + 1];
	double threshold;

	igt_debug("Launching %zd spinners on %s\n",
		  ARRAY_SIZE(ctx), class_to_str(ci->engine_class));
	igt_assert(ARRAY_SIZE(ctx) >= 3);

	for (int i = 0; i < ARRAY_SIZE(ctx); i++) {
		ctx[i] = load_balancer_create(i915, ci, count);
		if (spin == NULL) {
			spin = __igt_spin_new(i915, .ctx_id = ctx[i]);
		} else {
			struct drm_i915_gem_execbuffer2 eb = {
				.buffer_count = 1,
				.buffers_ptr = to_user_pointer(&spin->obj[IGT_SPIN_BATCH]),
				.rsvd1 = ctx[i],
			};
			gem_execbuf(i915, &eb);
		}
	}

	sleep(duration); /* over the course of many timeslices */

	igt_assert(gem_bo_busy(i915, spin->handle));
	igt_spin_end(spin);
	igt_debug("Cancelled spinners\n");

	for (int i = 0; i < ARRAY_SIZE(ctx); i++)
		ts[i] = read_ctx_timestamp(i915, ctx[i]);

	for (int i = 0; i < ARRAY_SIZE(ctx); i++)
		gem_context_destroy(i915, ctx[i]);
	igt_spin_free(i915, spin);

	/*
	 * If we imagine that the timeslices are randomly distributed to
	 * the virtual engines, we would expect the variation to be modelled
	 * by a drunken walk; ergo sqrt(num_timeslices).
	 */
	threshold = sqrt(1e9 * duration / timeslice_duration_ns);
	threshold *= timeslice_duration_ns;
	threshold *= 2; /* CI safety factor before crying wolf */

	qsort(ts, ARRAY_SIZE(ctx), sizeof(*ts), cmp_u32);
	igt_info("%s: [%.1f, %.1f, %.1f] ms, expect %1.f +- %.1fms\n",
		 class_to_str(ci->engine_class),
		 1e-6 * ticks_to_ns(i915, ts[0]),
		 1e-6 * ticks_to_ns(i915, ts[(count + 1) / 2]),
		 1e-6 * ticks_to_ns(i915, ts[count]),
		 2e3 * count / ARRAY_SIZE(ctx),
		 1e-6 * threshold);

	igt_assert_f(ts[count], "CTX_TIMESTAMP not reported!\n");
	igt_assert_f(ticks_to_ns(i915, ts[count] - ts[0]) < 2 * threshold,
		     "Range of timeslices greater than tolerable: %.2fms > %.2fms; unfair!\n",
		     1e-6 * ticks_to_ns(i915, ts[count] - ts[0]),
		     1e-6 * threshold * 2);
}

static void fairslice(int i915)
{
	/* Relative CS mmio */
	igt_require(intel_gen(intel_get_drm_devid(i915)) >= 11);

	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *ci;
		unsigned int count = 0;

		ci = list_engines(i915, 1u << class, &count);
		if (!ci || count < 2) {
			free(ci);
			continue;
		}

		__fairslice(i915, ci, count, 2);
		free(ci);
	}
}

static int wait_for_status(int fence, int timeout)
{
	int err;

	err = sync_fence_wait(fence, timeout);
	if (err)
		return err;

	return sync_fence_status(fence);
}

static void __persistence(int i915,
			  struct i915_engine_class_instance *ci,
			  unsigned int count,
			  bool persistent)
{
	igt_spin_t *spin;
	uint32_t ctx;

	/*
	 * A nonpersistent context is terminated immediately upon closure,
	 * any inflight request is cancelled.
	 */

	ctx = load_balancer_create(i915, ci, count);
	if (!persistent)
		gem_context_set_persistence(i915, ctx, persistent);

	spin = igt_spin_new(i915, ctx,
			    .flags = IGT_SPIN_FENCE_OUT | IGT_SPIN_POLL_RUN);
	igt_spin_busywait_until_started(spin);
	gem_context_destroy(i915, ctx);

	igt_assert_eq(wait_for_status(spin->out_fence, 500), -EIO);
	igt_spin_free(i915, spin);
}

static void persistence(int i915)
{
	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *ci;
		unsigned int count = 0;

		ci = list_engines(i915, 1u << class, &count);
		if (!ci || count < 2) {
			free(ci);
			continue;
		}

		__persistence(i915, ci, count, false);
		free(ci);
	}
}

static bool set_heartbeat(int i915, const char *name, unsigned int value)
{
	unsigned int x;

	if (gem_engine_property_printf(i915, name,
				       "heartbeat_interval_ms",
				       "%d", value) < 0)
		return false;

	x = ~value;
	gem_engine_property_scanf(i915, name,
				  "heartbeat_interval_ms",
				  "%d", &x);
	igt_assert_eq(x, value);

	return true;
}

static void noheartbeat(int i915)
{
	const struct intel_execution_engine2 *e;

	/*
	 * Check that non-persistent contexts are also cleaned up if we
	 * close the context while they are active, but the engine's
	 * heartbeat has already been disabled.
	 */

	for_each_physical_engine(i915, e)
		set_heartbeat(i915, e->name, 0);

	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *ci;
		unsigned int count = 0;

		ci = list_engines(i915, 1u << class, &count);
		if (!ci || count < 2) {
			free(ci);
			continue;
		}

		__persistence(i915, ci, count, true);
		free(ci);
	}

	igt_require_gem(i915); /* restore default parameters */
}

static bool enable_hangcheck(int dir, bool state)
{
	return igt_sysfs_set(dir, "enable_hangcheck", state ? "1" : "0");
}

static void nohangcheck(int i915)
{
	int params = igt_params_open(i915);

	igt_require(enable_hangcheck(params, false));

	for (int class = 0; class < 32; class++) {
		struct i915_engine_class_instance *ci;
		unsigned int count = 0;

		ci = list_engines(i915, 1u << class, &count);
		if (!ci || count < 2) {
			free(ci);
			continue;
		}

		__persistence(i915, ci, count, true);
		free(ci);
	}

	enable_hangcheck(params, true);
	close(params);
}

static bool has_persistence(int i915)
{
	struct drm_i915_gem_context_param p = {
		.param = I915_CONTEXT_PARAM_PERSISTENCE,
	};
	uint64_t saved;

	if (__gem_context_get_param(i915, &p))
		return false;

	saved = p.value;
	p.value = 0;
	if (__gem_context_set_param(i915, &p))
		return false;

	p.value = saved;
	return __gem_context_set_param(i915, &p) == 0;
}

static bool has_context_engines(int i915)
{
	struct drm_i915_gem_context_param p = {
		.param = I915_CONTEXT_PARAM_ENGINES,
	};

	return __gem_context_set_param(i915, &p) == 0;
}

static bool has_load_balancer(int i915)
{
	struct i915_engine_class_instance ci = {};
	uint32_t ctx;
	int err;

	ctx = gem_context_create(i915);
	err = __set_load_balancer(i915, ctx, &ci, 1, NULL);
	gem_context_destroy(i915, ctx);

	return err == 0;
}

static bool has_bonding(int i915)
{
	I915_DEFINE_CONTEXT_ENGINES_BOND(bonds, 0) = {
		.base.name = I915_CONTEXT_ENGINES_EXT_BOND,
	};
	struct i915_engine_class_instance ci = {};
	uint32_t ctx;
	int err;

	ctx = gem_context_create(i915);
	err = __set_load_balancer(i915, ctx, &ci, 1, &bonds);
	gem_context_destroy(i915, ctx);

	return err == 0;
}

igt_main
{
	int i915 = -1;

	igt_fixture {
		i915 = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(i915);

		gem_require_contexts(i915);
		igt_require(has_context_engines(i915));
		igt_require(has_load_balancer(i915));
		igt_require(has_perf_engines(i915));

		igt_fork_hang_detector(i915);
	}

	igt_subtest("invalid-balancer")
		invalid_balancer(i915);

	igt_subtest("invalid-bonds")
		invalid_bonds(i915);

	igt_subtest("individual")
		individual(i915);

	igt_subtest("indices")
		indices(i915);

	igt_subtest("busy")
		busy(i915);

	igt_subtest_group {
		static const struct {
			const char *name;
			unsigned int flags;
		} phases[] = {
			{ "", 0 },
			{ "-pulse", PULSE },
			{ "-late", LATE },
			{ "-late-pulse", PULSE | LATE },
			{ }
		};
		for (typeof(*phases) *p = phases; p->name; p++)
			igt_subtest_f("full%s", p->name)
				full(i915, p->flags);
	}

	igt_subtest("fairslice")
		fairslice(i915);

	igt_subtest("nop")
		nop(i915);

	igt_subtest("sequential")
		sequential(i915);

	igt_subtest("semaphore")
		semaphore(i915);

	igt_subtest("sliced")
		sliced(i915);

	igt_subtest("hog")
		hog(i915);

	igt_subtest("waits")
		waits(i915, 5);

	igt_subtest("smoke")
		smoketest(i915, 20);

	igt_subtest_group {
		igt_fixture igt_require(has_bonding(i915));

		igt_subtest("bonded-imm")
			bonded(i915, 0);

		igt_subtest("bonded-cork")
			bonded(i915, CORK);

		igt_subtest("bonded-early")
			bonded_early(i915);
	}

	igt_subtest("bonded-slice")
		bonded_slice(i915);

	igt_subtest("bonded-chain")
		bonded_chain(i915);

	igt_subtest("bonded-semaphore")
		bonded_semaphore(i915);

	igt_subtest("bonded-pair")
		bonded_runner(i915, __bonded_pair);
	igt_subtest("bonded-dual")
		bonded_runner(i915, __bonded_dual);
	igt_subtest("bonded-sync")
		bonded_runner(i915, __bonded_sync);

	igt_fixture {
		igt_stop_hang_detector();
	}

	igt_subtest_group {
		igt_hang_t  hang;

		igt_fixture
			hang = igt_allow_hang(i915, 0, 0);

		igt_subtest("bonded-false-hang")
			bonded_nohang(i915, NOHANG);

		igt_subtest("bonded-true-hang")
			bonded_nohang(i915, 0);

		igt_fixture
			igt_disallow_hang(i915, hang);

		igt_subtest("hang")
			hangme(i915);
	}

	igt_subtest_group {
		igt_fixture {
			igt_require_gem(i915); /* reset parameters */
			igt_require(has_persistence(i915));
		}

		igt_subtest("persistence")
			persistence(i915);

		igt_subtest("noheartbeat")
			noheartbeat(i915);

		igt_subtest("nohangcheck")
			nohangcheck(i915);
	}
}
