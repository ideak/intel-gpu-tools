/*
 * Copyright © 2016 Intel Corporation
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

#include <sys/poll.h>
#include <zlib.h>
#include <sched.h>

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt.h"
#include "igt_device.h"
#include "igt_rand.h"
#include "igt_sysfs.h"

#define MAX_RESET_TIME	600

IGT_TEST_DESCRIPTION("Check that we capture the user specified objects on a hang");

struct offset {
	uint64_t addr;
	unsigned long idx;
	bool found;
};

static unsigned long zlib_inflate(uint32_t **ptr, unsigned long len)
{
	struct z_stream_s zstream;
	void *out;

	memset(&zstream, 0, sizeof(zstream));

	zstream.next_in = (unsigned char *)*ptr;
	zstream.avail_in = 4*len;

	if (inflateInit(&zstream) != Z_OK)
		return 0;

	out = malloc(128*4096); /* approximate obj size */
	zstream.next_out = out;
	zstream.avail_out = 128*4096;

	do {
		switch (inflate(&zstream, Z_SYNC_FLUSH)) {
		case Z_STREAM_END:
			goto end;
		case Z_OK:
			break;
		default:
			inflateEnd(&zstream);
			return 0;
		}

		if (zstream.avail_out)
			break;

		out = realloc(out, 2*zstream.total_out);
		if (out == NULL) {
			inflateEnd(&zstream);
			return 0;
		}

		zstream.next_out = (unsigned char *)out + zstream.total_out;
		zstream.avail_out = zstream.total_out;
	} while (1);
end:
	inflateEnd(&zstream);
	free(*ptr);
	*ptr = out;
	return zstream.total_out / 4;
}

static unsigned long
ascii85_decode(char *in, uint32_t **out, bool inflate, char **end)
{
	unsigned long len = 0, size = 1024;

	*out = realloc(*out, sizeof(uint32_t)*size);
	if (*out == NULL)
		return 0;

	while (*in >= '!' && *in <= 'z') {
		uint32_t v = 0;

		if (len == size) {
			size *= 2;
			*out = realloc(*out, sizeof(uint32_t)*size);
			if (*out == NULL)
				return 0;
		}

		if (*in == 'z') {
			in++;
		} else {
			v += in[0] - 33; v *= 85;
			v += in[1] - 33; v *= 85;
			v += in[2] - 33; v *= 85;
			v += in[3] - 33; v *= 85;
			v += in[4] - 33;
			in += 5;
		}
		(*out)[len++] = v;
	}
	*end = in;

	if (!inflate)
		return len;

	return zlib_inflate(out, len);
}

static int check_error_state(int dir, struct offset *obj_offsets, int obj_count,
			     uint64_t obj_size, bool incremental)
{
	char *error, *str;
	int blobs = 0;

	errno = 0;
	error = igt_sysfs_get(dir, "error");
	igt_sysfs_set(dir, "error", "Begone!");
	igt_assert(error);
	igt_assert(errno != ENOMEM);
	igt_debug("%s\n", error);

	/* render ring --- user = 0x00000000 ffffd000 */
	for (str = error; (str = strstr(str, "--- user = ")); ) {
		uint32_t *data = NULL;
		uint64_t addr;
		unsigned long i, sz;
		unsigned long start;
		unsigned long end;

		if (strncmp(str, "--- user = 0x", 13))
			break;
		str += 13;
		addr = strtoul(str, &str, 16);
		addr <<= 32;
		addr |= strtoul(str + 1, &str, 16);
		igt_assert(*str++ == '\n');

		start = 0;
		end = obj_count;
		while (end > start) {
			i = (end - start) / 2 + start;
			if (obj_offsets[i].addr < addr)
				start = i + 1;
			else if (obj_offsets[i].addr > addr)
				end = i;
			else
				break;
		}
		igt_assert(obj_offsets[i].addr == addr);
		igt_assert(!obj_offsets[i].found);
		obj_offsets[i].found = true;
		igt_debug("offset:%"PRIx64", index:%ld\n",
			  addr, obj_offsets[i].idx);

		/* gtt_page_sizes = 0x00010000 */
		if (strncmp(str, "gtt_page_sizes = 0x", 19) == 0) {
			str += 19 + 8;
			igt_assert(*str++ == '\n');
		}

		if (!(*str == ':' || *str == '~'))
			continue;

		igt_debug("blob:%.64s\n", str);
		sz = ascii85_decode(str + 1, &data, *str == ':', &str);

		igt_assert_eq(4 * sz, obj_size);
		igt_assert(*str++ == '\n');
		str = strchr(str, '-');

		if (incremental) {
			uint32_t expect;

			expect = obj_offsets[i].idx * obj_size;
			for (i = 0; i < sz; i++)
				igt_assert_eq(data[i], expect++);
		} else {
			for (i = 0; i < sz; i++)
				igt_assert_eq(data[i], 0);
		}

		blobs++;
		free(data);
	}

	free(error);
	return blobs;
}

static struct gem_engine_properties
configure_hangs(int fd, const struct intel_execution_engine2 *e, int ctxt_id)
{
	struct gem_engine_properties props;

	/* Ensure fast hang detection */
	props.engine = e;
	props.preempt_timeout = 250;
	props.heartbeat_interval = 500;
	gem_engine_properties_configure(fd, &props);

	/* Allow engine based resets and disable banning */
	igt_allow_hang(fd, ctxt_id, HANG_ALLOW_CAPTURE | HANG_WANT_ENGINE_RESET);

	return props;
}

static bool fence_busy(int fence)
{
	return poll(&(struct pollfd){fence, POLLIN}, 1, 0) == 0;
}

static void wait_to_die(int fence_out)
{
	struct timeval before, after, delta;

	/* Wait for a reset to occur */
	gettimeofday(&before, NULL);
	while (fence_busy(fence_out)) {
		gettimeofday(&after, NULL);
		timersub(&after, &before, &delta);
		igt_assert(delta.tv_sec < MAX_RESET_TIME);
		sched_yield();
	}
	gettimeofday(&after, NULL);
	timersub(&after, &before, &delta);
	igt_info("Target died after %ld.%06lds\n", delta.tv_sec, delta.tv_usec);
}

static void __capture1(int fd, int dir, uint64_t ahnd, const intel_ctx_t *ctx,
		       const struct intel_execution_engine2 *e,
		       uint32_t target, uint64_t target_size, uint32_t region)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj[4];
#define SCRATCH 0
#define CAPTURE 1
#define NOCAPTURE 2
#define BATCH 3
	struct drm_i915_gem_relocation_entry reloc[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t *batch, *seqno;
	struct offset offset;
	int i, fence_out;
	struct gem_engine_properties saved_engine;

	saved_engine = configure_hangs(fd, e, ctx->id);

	memset(obj, 0, sizeof(obj));
	obj[SCRATCH].handle = gem_create_with_cpu_access_in_memory_regions(fd, 4096, region);
	obj[SCRATCH].flags = EXEC_OBJECT_WRITE;
	obj[CAPTURE].handle = target;
	obj[CAPTURE].flags = EXEC_OBJECT_CAPTURE;
	obj[NOCAPTURE].handle = gem_create(fd, 4096);

	obj[BATCH].handle = gem_create_with_cpu_access_in_memory_regions(fd, 4096, region);
	obj[BATCH].relocs_ptr = (uintptr_t)reloc;
	obj[BATCH].relocation_count = !ahnd ? ARRAY_SIZE(reloc) : 0;

	for (i = 0; i < ARRAY_SIZE(obj); i++) {
		obj[i].offset = get_offset(ahnd, obj[i].handle,
					   i == CAPTURE ? target_size : 4096, 0);
		obj[i].flags |= ahnd ? EXEC_OBJECT_PINNED : 0;
	}

	memset(reloc, 0, sizeof(reloc));
	reloc[0].target_handle = obj[BATCH].handle; /* recurse */
	reloc[0].presumed_offset = obj[BATCH].offset;
	reloc[0].offset = 5*sizeof(uint32_t);
	reloc[0].delta = 0;
	reloc[0].read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc[0].write_domain = 0;

	reloc[1].target_handle = obj[SCRATCH].handle; /* breadcrumb */
	reloc[1].presumed_offset = obj[SCRATCH].offset;
	reloc[1].offset = sizeof(uint32_t);
	reloc[1].delta = 0;
	reloc[1].read_domains = I915_GEM_DOMAIN_RENDER;
	reloc[1].write_domain = I915_GEM_DOMAIN_RENDER;

	seqno = gem_mmap__device_coherent(fd, obj[SCRATCH].handle, 0, 4096, PROT_READ);
	gem_set_domain(fd, obj[SCRATCH].handle,
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	batch = gem_mmap__cpu(fd, obj[BATCH].handle, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, obj[BATCH].handle,
			I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	i = 0;
	batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		batch[++i] = obj[SCRATCH].offset;
		batch[++i] = obj[SCRATCH].offset >> 32;
	} else if (gen >= 4) {
		batch[++i] = 0;
		batch[++i] = 0;
		reloc[1].offset += sizeof(uint32_t);
	} else {
		batch[i]--;
		batch[++i] = 0;
	}
	batch[++i] = 0xc0ffee;
	if (gen < 4)
		batch[++i] = MI_NOOP;

	batch[++i] = MI_BATCH_BUFFER_START; /* not crashed? try again! */
	if (gen >= 8) {
		batch[i] |= 1 << 8 | 1;
		batch[++i] = obj[BATCH].offset;
		batch[++i] = obj[BATCH].offset >> 32;
	} else if (gen >= 6) {
		batch[i] |= 1 << 8;
		batch[++i] = 0;
	} else {
		batch[i] |= 2 << 6;
		batch[++i] = 0;
		if (gen < 4) {
			batch[i] |= 1;
			reloc[0].delta = 1;
		}
	}
	munmap(batch, 4096);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)obj;
	execbuf.buffer_count = ARRAY_SIZE(obj);
	execbuf.flags = e->flags;
	if (gen > 3 && gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;
	execbuf.flags |= I915_EXEC_FENCE_OUT;
	execbuf.rsvd1 = ctx->id;
	execbuf.rsvd2 = ~0UL;

	igt_assert(!READ_ONCE(*seqno));
	gem_execbuf_wr(fd, &execbuf);

	fence_out = execbuf.rsvd2 >> 32;
	igt_assert(fence_out >= 0);

	/* Wait for the request to start */
	while (READ_ONCE(*seqno) != 0xc0ffee)
		igt_assert(gem_bo_busy(fd, obj[SCRATCH].handle));
	munmap(seqno, 4096);

	/* Wait for a reset to occur */
	wait_to_die(fence_out);

	/* Check that only the buffer we marked is reported in the error */
	memset(&offset, 0, sizeof(offset));
	offset.addr = obj[CAPTURE].offset;
	igt_assert_eq(check_error_state(dir, &offset, 1, target_size, false), 1);
	igt_assert(offset.found);

	gem_sync(fd, obj[BATCH].handle);

	for (i = 0; i < ARRAY_SIZE(obj); i++)
		put_offset(ahnd, obj[i].handle);
	gem_close(fd, obj[BATCH].handle);
	gem_close(fd, obj[NOCAPTURE].handle);
	gem_close(fd, obj[SCRATCH].handle);

	gem_engine_properties_restore(fd, &saved_engine);
}

static void capture(int fd, int dir, const intel_ctx_t *ctx,
		    const struct intel_execution_engine2 *e, uint32_t region)
{
	uint32_t handle;
	uint64_t ahnd, obj_size = 4096;

	igt_assert_eq(__gem_create_with_cpu_access_in_memory_regions(fd, &handle, &obj_size, region), 0);
	ahnd = get_reloc_ahnd(fd, ctx->id);

	__capture1(fd, dir, ahnd, ctx, e, handle, obj_size, region);

	gem_close(fd, handle);
	put_ahnd(ahnd);
}

static int cmp(const void *A, const void *B)
{
	const uint64_t *a = A, *b = B;

	if (*a < *b)
		return -1;

	if (*a > *b)
		return 1;

	return 0;
}

static struct offset *
__captureN(int fd, int dir, uint64_t ahnd, const intel_ctx_t *ctx,
	   const struct intel_execution_engine2 *e,
	   unsigned int size, int count,
	   unsigned int flags, int *_fence_out, uint32_t region,
	   bool force_cpu_access)
#define INCREMENTAL 0x1
#define ASYNC 0x2
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 *obj;
	struct drm_i915_gem_relocation_entry reloc[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t *batch, *seqno;
	struct offset *offsets;
	int i, fence_out;
	struct gem_engine_properties saved_engine;

	saved_engine = configure_hangs(fd, e, ctx->id);

	offsets = calloc(count, sizeof(*offsets));
	igt_assert(offsets);

	obj = calloc(count + 2, sizeof(*obj));
	igt_assert(obj);

	obj[0].handle = gem_create(fd, 4096);
	obj[0].offset = get_offset(ahnd, obj[0].handle, 4096, 0);
	obj[0].flags = EXEC_OBJECT_WRITE | (ahnd ? EXEC_OBJECT_PINNED : 0);

	for (i = 0; i < count; i++) {
		if (force_cpu_access)
			obj[i + 1].handle = gem_create_with_cpu_access_in_memory_regions(fd, size, region);
		else
			obj[i + 1].handle = gem_create_in_memory_regions(fd, size, region);
		obj[i + 1].offset = get_offset(ahnd, obj[i + 1].handle, size, 0);
		obj[i + 1].flags =
			EXEC_OBJECT_CAPTURE | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
		if (ahnd)
			obj[i + 1].flags |= EXEC_OBJECT_PINNED;

		if (flags & INCREMENTAL) {
			uint32_t *ptr;

			ptr = gem_mmap__cpu(fd, obj[i + 1].handle,
					    0, size, PROT_WRITE);
			for (unsigned int n = 0; n < size / sizeof(*ptr); n++)
				ptr[n] = i * size + n;
			munmap(ptr, size);
		}
	}

	obj[count + 1].handle = gem_create(fd, 4096);
	obj[count + 1].relocs_ptr = (uintptr_t)reloc;
	obj[count + 1].relocation_count = !ahnd ? ARRAY_SIZE(reloc) : 0;
	obj[count + 1].offset = get_offset(ahnd, obj[count + 1].handle, 4096, 0);
	obj[count + 1].flags = ahnd ? EXEC_OBJECT_PINNED : 0;

	memset(reloc, 0, sizeof(reloc));
	reloc[0].target_handle = obj[count + 1].handle; /* recurse */
	reloc[0].presumed_offset = obj[count + 1].offset;
	reloc[0].offset = 5*sizeof(uint32_t);
	reloc[0].delta = 0;
	reloc[0].read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc[0].write_domain = 0;

	reloc[1].target_handle = obj[0].handle; /* breadcrumb */
	reloc[1].presumed_offset = obj[0].offset;
	reloc[1].offset = sizeof(uint32_t);
	reloc[1].delta = 0;
	reloc[1].read_domains = I915_GEM_DOMAIN_RENDER;
	reloc[1].write_domain = I915_GEM_DOMAIN_RENDER;

	if (!ahnd) {
		obj[count + 1].relocs_ptr = (uintptr_t)reloc;
		obj[count + 1].relocation_count = ARRAY_SIZE(reloc);
	}

	seqno = gem_mmap__device_coherent(fd, obj[0].handle, 0, 4096, PROT_READ);
	gem_set_domain(fd, obj[0].handle,
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	batch = gem_mmap__cpu(fd, obj[count + 1].handle, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, obj[count + 1].handle,
			I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	i = 0;
	batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		batch[++i] = obj[0].offset;
		batch[++i] = obj[0].offset >> 32;
	} else if (gen >= 4) {
		batch[++i] = 0;
		batch[++i] = 0;
		reloc[1].offset += sizeof(uint32_t);
	} else {
		batch[i]--;
		batch[++i] = 0;
	}
	batch[++i] = 0xc0ffee;
	if (gen < 4)
		batch[++i] = MI_NOOP;

	batch[++i] = MI_BATCH_BUFFER_START; /* not crashed? try again! */
	if (gen >= 8) {
		batch[i] |= 1 << 8 | 1;
		batch[++i] = obj[count + 1].offset;
		batch[++i] = obj[count + 1].offset >> 32;
	} else if (gen >= 6) {
		batch[i] |= 1 << 8;
		batch[++i] = 0;
	} else {
		batch[i] |= 2 << 6;
		batch[++i] = 0;
		if (gen < 4) {
			batch[i] |= 1;
			reloc[0].delta = 1;
		}
	}
	munmap(batch, 4096);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)obj;
	execbuf.buffer_count = count + 2;
	execbuf.flags = e->flags;
	if (gen > 3 && gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;
	execbuf.flags |= I915_EXEC_FENCE_OUT;
	execbuf.rsvd1 = ctx->id;
	execbuf.rsvd2 = ~0UL;

	igt_assert(!READ_ONCE(*seqno));
	gem_execbuf_wr(fd, &execbuf);

	fence_out = execbuf.rsvd2 >> 32;
	igt_assert(fence_out >= 0);
	if (_fence_out)
		*_fence_out = fence_out;

	/* Wait for the request to start */
	while (READ_ONCE(*seqno) != 0xc0ffee)
		igt_assert(gem_bo_busy(fd, obj[0].handle));
	munmap(seqno, 4096);

	if (!(flags & ASYNC)) {
		wait_to_die(fence_out);
		gem_sync(fd, obj[count + 1].handle);
	}

	gem_close(fd, obj[count + 1].handle);
	put_offset(ahnd, obj[count + 1].handle);
	for (i = 0; i < count; i++) {
		offsets[i].addr = obj[i + 1].offset;
		offsets[i].idx = i;
		gem_close(fd, obj[i + 1].handle);
		put_offset(ahnd, obj[i + 1].handle);
	}
	gem_close(fd, obj[0].handle);
	put_offset(ahnd, obj[0].handle);

	qsort(offsets, count, sizeof(*offsets), cmp);
	igt_assert(offsets[0].addr <= offsets[count-1].addr);

	gem_engine_properties_restore(fd, &saved_engine);
	return offsets;
}

static bool kernel_supports_probed_size(int fd)
{
	struct drm_i915_query_memory_regions *regions;
	int i, ret = false;

	regions = gem_get_query_memory_regions(fd);
	igt_assert(regions);
	igt_assert(regions->num_regions);

	for (i = 0; i < regions->num_regions; i++) {
		struct drm_i915_memory_region_info info = regions->regions[i];

		if (info.probed_cpu_visible_size) {
			ret = true;
			break;
		}
	}

	free(regions);
	return ret;
}

static bool needs_recoverable_ctx(int fd)
{
	uint16_t devid;

	if (!kernel_supports_probed_size(fd))
		return false;

	devid = intel_get_drm_devid(fd);
	return gem_has_lmem(fd) ||  intel_graphics_ver(devid) > IP_VER(12, 0);
}

#define find_first_available_engine(fd, ctx, e, saved) \
	do { \
		ctx = intel_ctx_create_all_physical(fd); \
		igt_assert(ctx); \
		for_each_ctx_engine(fd, ctx, e) \
			for_each_if(gem_class_can_store_dword(fd, e->class)) \
				break; \
		igt_assert(e); \
		saved = configure_hangs(fd, e, ctx->id); \
	} while(0)

static void many(int fd, int dir, uint64_t size, unsigned int flags)
{
	const struct intel_execution_engine2 *e;
	const intel_ctx_t *ctx;
	uint64_t ram, gtt, ahnd;
	unsigned long count, blobs;
	struct offset *offsets;
	struct gem_engine_properties saved_engine;

	find_first_available_engine(fd, ctx, e, saved_engine);
	if (needs_recoverable_ctx(fd)) {
		struct drm_i915_gem_context_param param = {
			.ctx_id = ctx->id,
			.param = I915_CONTEXT_PARAM_RECOVERABLE,
			.value = 0,
		};

		gem_context_set_param(fd, &param);
	}

	gtt = gem_aperture_size(fd) / size;
	ram = (igt_get_avail_ram_mb() << 20) / size;
	igt_debug("Available objects in GTT:%"PRIu64", RAM:%"PRIu64"\n",
		  gtt, ram);

	count = min(gtt, ram) / 4;
	igt_require(count > 1);

	igt_require_memory(count, size, CHECK_RAM);
	ahnd = get_reloc_ahnd(fd, ctx->id);

	offsets = __captureN(fd, dir, ahnd, ctx, e, size, count, flags, NULL,
			     REGION_SMEM, true);

	blobs = check_error_state(dir, offsets, count, size, !!(flags & INCREMENTAL));
	igt_info("Captured %lu %"PRId64"-blobs out of a total of %lu\n",
		 blobs, size >> 12, count);

	free(offsets);
	put_ahnd(ahnd);

	gem_engine_properties_restore(fd, &saved_engine);
}

static void prioinv(int fd, int dir, const intel_ctx_t *ctx,
		    const struct intel_execution_engine2 *e)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj = {
		.handle = gem_create(fd, 4096),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = e->flags,
		.rsvd1 = ctx->id,
	};
	struct gem_engine_properties saved_engine;
	int64_t timeout = NSEC_PER_SEC; /* 1s, feeling generous, blame debug */
	uint64_t ram, gtt, ahnd, size = 4 << 20;
	unsigned long count;
	int link[2], dummy;

	ahnd = get_reloc_ahnd(fd, ctx->id);
	obj.offset = get_offset(ahnd, obj.handle, 4096, 0);

	igt_require(gem_scheduler_enabled(fd));
	igt_require(igt_params_set(fd, "reset", "%u", -1)); /* engine resets! */
	igt_require(gem_gpu_reset_type(fd) > 1);

	gtt = gem_aperture_size(fd) / size;
	ram = (igt_get_avail_ram_mb() << 20) / size;
	igt_debug("Available objects in GTT:%"PRIu64", RAM:%"PRIu64"\n",
		  gtt, ram);

	count = min(gtt, ram) / 4;
	count = min(count, 256ul); /* Keep the duration within reason */
	igt_require(count > 1);

	igt_require_memory(count, size, CHECK_RAM);

	saved_engine = configure_hangs(fd, e, ctx->id);

	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));
	gem_execbuf(fd, &execbuf);
	gem_sync(fd, obj.handle);

	igt_assert(pipe(link) == 0);
	igt_fork(child, 1) {
		const intel_ctx_t *ctx2;
		int fence_out;
		fd = gem_reopen_driver(fd);
		igt_debug("Submitting large capture [%ld x %dMiB objects]\n",
			  count, (int)(size >> 20));

		ctx2 = intel_ctx_create_all_physical(fd);
		igt_assert(ctx2);
		if (needs_recoverable_ctx(fd)) {
			struct drm_i915_gem_context_param param = {
				.ctx_id = ctx2->id,
				.param = I915_CONTEXT_PARAM_RECOVERABLE,
				.value = 0,
			};

			gem_context_set_param(fd, &param);
		}

		intel_allocator_init();
		/* Reopen the allocator in the new process. */
		ahnd = get_reloc_ahnd(fd, ctx2->id);

		free(__captureN(fd, dir, ahnd, ctx2, e, size, count, ASYNC,
				&fence_out, REGION_SMEM, true));
		put_ahnd(ahnd);

		write(link[1], &fd, sizeof(fd)); /* wake the parent up */
		wait_to_die(fence_out);
		write(link[1], &fd, sizeof(fd)); /* wake the parent up */
	}
	read(link[0], &dummy, sizeof(dummy));
	igt_require_f(poll(&(struct pollfd){link[0], POLLIN}, 1, 500) == 0,
		      "Capture completed too quickly! Will not block\n");

	igt_debug("Submitting nop\n");
	gem_execbuf(fd, &execbuf);
	igt_assert_eq(gem_wait(fd, obj.handle, &timeout), 0);
	gem_close(fd, obj.handle);

	igt_assert_f(poll(&(struct pollfd){link[0], POLLIN}, 1, 0) == 0,
		     "Capture completed before nop!\n");

	igt_debug("Waiting for capture/reset to complete\n");
	igt_waitchildren();
	close(link[0]);
	close(link[1]);

	gem_engine_properties_restore(fd, &saved_engine);

	gem_quiescent_gpu(fd);
	put_offset(ahnd, obj.handle);
	put_ahnd(ahnd);
}

static void userptr(int fd, int dir)
{
	const struct intel_execution_engine2 *e;
	const intel_ctx_t *ctx;
	uint32_t handle;
	uint64_t ahnd;
	void *ptr;
	int obj_size = 4096;
	uint32_t system_region = INTEL_MEMORY_REGION_ID(I915_SYSTEM_MEMORY, 0);
	struct gem_engine_properties saved_engine;

	find_first_available_engine(fd, ctx, e, saved_engine);
	if (needs_recoverable_ctx(fd)) {
		struct drm_i915_gem_context_param param = {
			.ctx_id = ctx->id,
			.param = I915_CONTEXT_PARAM_RECOVERABLE,
			.value = 0,
		};

		gem_context_set_param(fd, &param);
	}

	igt_assert(posix_memalign(&ptr, obj_size, obj_size) == 0);
	memset(ptr, 0, obj_size);
	igt_require(__gem_userptr(fd, ptr, obj_size, 0, 0, &handle) == 0);
	ahnd = get_reloc_ahnd(fd, ctx->id);

	__capture1(fd, dir, ahnd, ctx, e, handle, obj_size, system_region);

	gem_close(fd, handle);
	put_ahnd(ahnd);
	free(ptr);

	gem_engine_properties_restore(fd, &saved_engine);
}

static uint32_t batch_create_size(int fd, uint64_t size)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle;

	handle = gem_create(fd, size);
	gem_write(fd, handle, 0, &bbe, sizeof(bbe));

	return handle;
}

static void capture_recoverable_discrete(int fd)
{
	struct drm_i915_gem_exec_object2 exec[2] = {};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&exec),
		.buffer_count = 2,
	};

	/*
	 * I915_CONTEXT_PARAM_RECOVERABLE should be enabled by default. On
	 * discrete the kernel will only capture objects associated with the
	 * batch, if the context we is configured as non-recoverable.
	 */

	exec[0].handle = gem_create(fd, 4096);
	exec[0].flags = EXEC_OBJECT_CAPTURE;
	exec[1].handle = batch_create_size(fd, 4096);

	igt_assert_neq(__gem_execbuf(fd, &execbuf), 0);
}

static void capture_invisible(int fd, int dir, const intel_ctx_t *ctx,
			      struct gem_memory_region *mr)
{
	struct gem_engine_properties saved_engine;
	struct drm_i915_gem_context_param param = {
		.param = I915_CONTEXT_PARAM_RECOVERABLE,
		.value = 0,
	};
	const struct intel_execution_engine2 *e;
	struct offset *offsets;
	uint64_t ahnd;
	char *error;

	find_first_available_engine(fd, ctx, e, saved_engine);
	param.ctx_id = ctx->id,
	gem_context_set_param(fd, &param);

	ahnd = get_reloc_ahnd(fd, ctx->id);

	igt_assert_eq(mr->ci.memory_class, I915_MEMORY_CLASS_DEVICE);

	offsets = __captureN(fd, dir, ahnd, ctx, e, 1u << 16, 100, 0, NULL,
			     INTEL_MEMORY_REGION_ID(mr->ci.memory_class,
						    mr->ci.memory_instance),
			     false);

	/*
	 * Make sure the error capture code doesn't crash-and-burn if it
	 * encounters an lmem object that can't be copied using the CPU. In such
	 * cases such objects will be skipped, otherwise we should see crashes
	 * here.  Allocating a number of small objects should be enough to
	 * ensure that at least one or more end being allocated in the CPU
	 * invisible portion.
	 */

	error = igt_sysfs_get(dir, "error");
	igt_sysfs_set(dir, "error", "Begone!");
	igt_assert(error);
	igt_assert(errno != ENOMEM);

	gem_engine_properties_restore(fd, &saved_engine);

	free(offsets);
	put_ahnd(ahnd);
}

static bool has_capture(int fd)
{
	drm_i915_getparam_t gp;
	int async = -1;

	gp.param = I915_PARAM_HAS_EXEC_CAPTURE;
	gp.value = &async;
	drmIoctl(fd, DRM_IOCTL_I915_GETPARAM, &gp);

	return async > 0;
}

static size_t safer_strlen(const char *s)
{
	return s ? strlen(s) : 0;
}

#define test_each_engine(T, i915, ctx, e) \
	igt_subtest_with_dynamic(T) for_each_ctx_engine(i915, ctx, e) \
		for_each_if(gem_class_can_store_dword(i915, (e)->class)) \

igt_main
{
	const struct intel_execution_engine2 *e;
	const intel_ctx_t *ctx;
	igt_hang_t hang;
	int fd = -1;
	int dir = -1;
	struct drm_i915_query_memory_regions *query_info;
	struct igt_collection *regions, *set;
	char *sub_name;
	uint32_t region;

	igt_fixture {
		int gen;

		fd = drm_open_driver(DRIVER_INTEL);

		gen = intel_gen(intel_get_drm_devid(fd));
		if (gen > 3 && gen < 6) /* ctg and ilk need secure batches */
			igt_device_set_master(fd);

		igt_require_gem(fd);
		gem_require_mmap_device_coherent(fd);
		igt_require(has_capture(fd));
		ctx = intel_ctx_create_all_physical(fd);
		if (needs_recoverable_ctx(fd)) {
			struct drm_i915_gem_context_param param = {
				.ctx_id = ctx->id,
				.param = I915_CONTEXT_PARAM_RECOVERABLE,
				.value = 0,
			};

			gem_context_set_param(fd, &param);
		}
		igt_allow_hang(fd, ctx->id, HANG_ALLOW_CAPTURE | HANG_WANT_ENGINE_RESET);

		dir = igt_sysfs_open(fd);
		igt_require(igt_sysfs_set(dir, "error", "Begone!"));
		igt_require(safer_strlen(igt_sysfs_get(dir, "error")) > 0);
		query_info = gem_get_query_memory_regions(fd);
		igt_assert(query_info);
		set = get_memory_region_set(query_info,
				I915_SYSTEM_MEMORY,
				I915_DEVICE_MEMORY);
	}

	test_each_engine("capture", fd, ctx, e) {
		for_each_combination(regions, 1, set) {
			sub_name = memregion_dynamic_subtest_name(regions);
			region = igt_collection_get_value(regions, 0);
			igt_dynamic_f("%s-%s", e->name, sub_name)
				capture(fd, dir, ctx, e, region);
			free(sub_name);
		}
	}

	igt_describe("Check that the kernel doesn't crash if the pages can't be copied from the CPU during error capture.");
	igt_subtest_with_dynamic("capture-invisible") {
		for_each_memory_region(r, fd) {
			igt_dynamic_f("%s", r->name) {
				igt_require(r->cpu_size && r->cpu_size < r->size);
				capture_invisible(fd, dir, ctx, r);
			}
		}
	}

	igt_describe("Verify that the kernel rejects EXEC_OBJECT_CAPTURE with recoverable contexts.");
	igt_subtest_f("capture-recoverable") {
		igt_require(needs_recoverable_ctx(fd));
		capture_recoverable_discrete(fd);
	}

	igt_subtest_f("many-4K-zero") {
		igt_require(gem_can_store_dword(fd, 0));
		many(fd, dir, 1<<12, 0);
	}

	igt_subtest_f("many-4K-incremental") {
		igt_require(gem_can_store_dword(fd, 0));
		many(fd, dir, 1<<12, INCREMENTAL);
	}

	igt_subtest_f("many-2M-zero") {
		igt_require(gem_can_store_dword(fd, 0));
		many(fd, dir, 2<<20, 0);
	}

	igt_subtest_f("many-2M-incremental") {
		igt_require(gem_can_store_dword(fd, 0));
		many(fd, dir, 2<<20, INCREMENTAL);
	}

	igt_subtest_f("many-256M-incremental") {
		igt_require(gem_can_store_dword(fd, 0));
		many(fd, dir, 256<<20, INCREMENTAL);
	}

	/* And check we can read from different types of objects */

	igt_subtest_f("userptr") {
		igt_require(gem_can_store_dword(fd, 0));
		userptr(fd, dir);
	}

	test_each_engine("pi", fd, ctx, e)
		igt_dynamic_f("%s", (e)->name)
			prioinv(fd, dir, ctx, e);

	igt_fixture {
		close(dir);
		igt_disallow_hang(fd, hang);
		intel_ctx_destroy(fd, ctx);
		close(fd);
	}
}
