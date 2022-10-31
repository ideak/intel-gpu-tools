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

#include "igt.h"
#include "intel_hwconfig_types.h"
#include "i915/gem.h"
#include "i915/gem_create.h"

#include <limits.h>

IGT_TEST_DESCRIPTION("Testing the i915 query uAPI.");

/*
 * We should at least get 3 bytes for data for each slices, subslices & EUs
 * masks.
 */
#define MIN_TOPOLOGY_ITEM_SIZE (sizeof(struct drm_i915_query_topology_info) + 3)

/* All devices should have at least one region. */
#define MIN_REGIONS_ITEM_SIZE (sizeof(struct drm_i915_query_memory_regions) + \
			       sizeof(struct drm_i915_memory_region_info))

static int
__i915_query(int fd, struct drm_i915_query *q)
{
	if (igt_ioctl(fd, DRM_IOCTL_I915_QUERY, q))
		return -errno;
	return 0;
}

static int
__i915_query_items(int fd, struct drm_i915_query_item *items, uint32_t n_items)
{
	struct drm_i915_query q = {
		.num_items = n_items,
		.items_ptr = to_user_pointer(items),
	};
	return __i915_query(fd, &q);
}

#define i915_query_items(fd, items, n_items) do { \
		igt_assert_eq(__i915_query_items(fd, items, n_items), 0); \
		errno = 0; \
	} while (0)
#define i915_query_items_err(fd, items, n_items, err) do { \
		igt_assert_eq(__i915_query_items(fd, items, n_items), -err); \
	} while (0)

static bool has_query_supports(int fd)
{
	struct drm_i915_query query = {};

	return __i915_query(fd, &query) == 0;
}

static void test_query_garbage(int fd)
{
	struct drm_i915_query query;
	struct drm_i915_query_item item;

	/* Verify that invalid query pointers are rejected. */
	igt_assert_eq(__i915_query(fd, NULL), -EFAULT);
	igt_assert_eq(__i915_query(fd, (void *) -1), -EFAULT);

	/*
	 * Query flags field is currently valid only if equals to 0. This might
	 * change in the future.
	 */
	memset(&query, 0, sizeof(query));
	query.flags = 42;
	igt_assert_eq(__i915_query(fd, &query), -EINVAL);

	/* Test a couple of invalid pointers. */
	i915_query_items_err(fd, (void *) ULONG_MAX, 1, EFAULT);
	i915_query_items_err(fd, (void *) 0, 1, EFAULT);

	/* Test the invalid query id = 0. */
	memset(&item, 0, sizeof(item));
	i915_query_items_err(fd, &item, 1, EINVAL);
}

static void test_query_garbage_items(int fd, int query_id, int min_item_size,
				     int sizeof_query_item)
{
	struct drm_i915_query_item items[2];
	struct drm_i915_query_item *items_ptr;
	int i, n_items;

	/*
	 * Query item flags field is currently valid only if equals to 0.
	 * Subject to change in the future.
	 */
	memset(items, 0, sizeof(items));
	items[0].query_id = query_id;
	items[0].flags = 42;
	i915_query_items(fd, items, 1);
	igt_assert_eq(items[0].length, -EINVAL);

	/*
	 * Test an invalid query id in the second item and verify that the first
	 * one is properly processed.
	 */
	memset(items, 0, sizeof(items));
	items[0].query_id = query_id;
	items[1].query_id = ULONG_MAX;
	i915_query_items(fd, items, 2);
	igt_assert_lte(min_item_size, items[0].length);
	igt_assert_eq(items[1].length, -EINVAL);

	/*
	 * Test a invalid query id in the first item and verify that the second
	 * one is properly processed (the driver is expected to go through them
	 * all and place error codes in the failed items).
	 */
	memset(items, 0, sizeof(items));
	items[0].query_id = ULONG_MAX;
	items[1].query_id = query_id;
	i915_query_items(fd, items, 2);
	igt_assert_eq(items[0].length, -EINVAL);
	igt_assert_lte(min_item_size, items[1].length);

	/* Test a couple of invalid data pointer in query item. */
	memset(items, 0, sizeof(items));
	items[0].query_id = query_id;
	i915_query_items(fd, items, 1);
	igt_assert_lte(min_item_size, items[0].length);

	items[0].data_ptr = 0;
	i915_query_items(fd, items, 1);
	igt_assert_eq(items[0].length, -EFAULT);

	items[0].data_ptr = ULONG_MAX;
	i915_query_items(fd, items, 1);
	igt_assert_eq(items[0].length, -EFAULT);

	/* Test an invalid query item length. */
	memset(items, 0, sizeof(items));
	items[0].query_id = query_id;
	items[1].query_id = query_id;
	items[1].length = sizeof_query_item - 1;
	i915_query_items(fd, items, 2);
	igt_assert_lte(min_item_size, items[0].length);
	igt_assert_eq(items[1].length, -EINVAL);

	/*
	 * Map memory for a query item in which the kernel is going to write the
	 * length of the item in the first ioctl(). Then unmap that memory and
	 * verify that the kernel correctly returns EFAULT as memory of the item
	 * has been removed from our address space.
	 */
	items_ptr = mmap(0, 4096, PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	items_ptr[0].query_id = query_id;
	i915_query_items(fd, items_ptr, 1);
	igt_assert_lte(min_item_size, items_ptr[0].length);
	munmap(items_ptr, 4096);
	i915_query_items_err(fd, items_ptr, 1, EFAULT);

	/*
	 * Map memory for a query item, then make it read only and verify that
	 * the kernel errors out with EFAULT.
	 */
	items_ptr = mmap(0, 4096, PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	items_ptr[0].query_id = query_id;
	igt_assert_eq(0, mprotect(items_ptr, 4096, PROT_READ));
	i915_query_items_err(fd, items_ptr, 1, EFAULT);
	munmap(items_ptr, 4096);

	/*
	 * Allocate 2 pages, prepare those 2 pages with valid query items, then
	 * switch the second page to read only and expect an EFAULT error.
	 */
	items_ptr = mmap(0, 8192, PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	memset(items_ptr, 0, 8192);
	n_items = 8192 / sizeof(struct drm_i915_query_item);
	for (i = 0; i < n_items; i++)
		items_ptr[i].query_id = query_id;
	mprotect(((uint8_t *)items_ptr) + 4096, 4096, PROT_READ);
	i915_query_items_err(fd, items_ptr, n_items, EFAULT);
	munmap(items_ptr, 8192);
}

static void test_query_topology_garbage_items(int fd)
{
	test_query_garbage_items(fd,
				 DRM_I915_QUERY_TOPOLOGY_INFO,
				 MIN_TOPOLOGY_ITEM_SIZE,
				 sizeof(struct drm_i915_query_topology_info));
}

/*
 * Allocate more on both sides of where the kernel is going to write and verify
 * that it writes only where it's supposed to.
 */
static void test_query_topology_kernel_writes(int fd)
{
	struct drm_i915_query_item item;
	struct drm_i915_query_topology_info *topo_info;
	uint8_t *_topo_info;
	int b, total_size;

	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_TOPOLOGY_INFO;
	i915_query_items(fd, &item, 1);
	igt_assert_lte(MIN_TOPOLOGY_ITEM_SIZE, item.length);

	total_size = item.length + 2 * sizeof(*_topo_info);
	_topo_info = malloc(total_size);
	memset(_topo_info, 0xff, total_size);
	topo_info = (struct drm_i915_query_topology_info *) (_topo_info + sizeof(*_topo_info));
	memset(topo_info, 0, item.length);

	item.data_ptr = to_user_pointer(topo_info);
	i915_query_items(fd, &item, 1);

	for (b = 0; b < sizeof(*_topo_info); b++) {
		igt_assert_eq(_topo_info[b], 0xff);
		igt_assert_eq(_topo_info[sizeof(*_topo_info) + item.length + b], 0xff);
	}
}

static bool query_topology_supported(int fd)
{
	struct drm_i915_query_item item = {
		.query_id = DRM_I915_QUERY_TOPOLOGY_INFO,
	};

	return __i915_query_items(fd, &item, 1) == 0 && item.length > 0;
}

static bool query_geometry_subslices_supported(int fd)
{
	struct drm_i915_query_item item = {
		.query_id= DRM_I915_QUERY_GEOMETRY_SUBSLICES,
	};

	return __i915_query_items(fd, &item, 1) == 0 && item.length > 0;
}

static void test_query_topology_unsupported(int fd)
{
	struct drm_i915_query_item item = {
		.query_id = DRM_I915_QUERY_TOPOLOGY_INFO,
	};

	i915_query_items(fd, &item, 1);
	igt_assert_eq(item.length, -ENODEV);
}

static bool
slice_available(const struct drm_i915_query_topology_info *topo_info,
		int s)
{
	return (topo_info->data[s / 8] >> (s % 8)) & 1;
}

static bool
subslice_available(const struct drm_i915_query_topology_info *topo_info,
		   int s, int ss)
{
	return (topo_info->data[topo_info->subslice_offset +
				s * topo_info->subslice_stride +
				ss / 8] >> (ss % 8)) & 1;
}

static bool
eu_available(const struct drm_i915_query_topology_info *topo_info,
	     int s, int ss, int eu)
{
	return (topo_info->data[topo_info->eu_offset +
				(s * topo_info->max_subslices + ss) * topo_info->eu_stride +
				eu / 8] >> (eu % 8)) & 1;
}

/*
 * Verify that we get coherent values between the legacy getparam slice/subslice
 * masks and the new topology query.
 */
static void
test_query_topology_coherent_slice_mask(int fd)
{
	struct drm_i915_query_item item;
	struct drm_i915_query_topology_info *topo_info;
	drm_i915_getparam_t gp;
	int slice_mask, subslice_mask;
	int s, topology_slices, topology_subslices_slice0;
	int32_t first_query_length;

	gp.param = I915_PARAM_SLICE_MASK;
	gp.value = &slice_mask;
	igt_skip_on(igt_ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp) != 0);

	gp.param = I915_PARAM_SUBSLICE_MASK;
	gp.value = &subslice_mask;
	igt_skip_on(igt_ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp) != 0);

	/* Slices */
	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_TOPOLOGY_INFO;
	i915_query_items(fd, &item, 1);
	/* We expect at least one byte for each slices, subslices & EUs masks. */
	igt_assert_lte(MIN_TOPOLOGY_ITEM_SIZE, item.length);
	first_query_length = item.length;

	topo_info = calloc(1, item.length);

	item.data_ptr = to_user_pointer(topo_info);
	i915_query_items(fd, &item, 1);
	/* We should get the same size once the data has been written. */
	igt_assert_eq(first_query_length, item.length);
	/* We expect at least one byte for each slices, subslices & EUs masks. */
	igt_assert_lte(MIN_TOPOLOGY_ITEM_SIZE, item.length);

	topology_slices = 0;
	for (s = 0; s < topo_info->max_slices; s++) {
		if (slice_available(topo_info, s))
			topology_slices |= 1UL << s;
	}

	igt_debug("slice mask getparam=0x%x / query=0x%x\n",
		  slice_mask, topology_slices);

	/* These 2 should always match. */
	igt_assert_eq(slice_mask, topology_slices);

	topology_subslices_slice0 = 0;
	for (s = 0; s < topo_info->max_subslices; s++) {
		if (subslice_available(topo_info, 0, s))
			topology_subslices_slice0 |= 1UL << s;
	}

	igt_debug("subslice mask getparam=0x%x / query=0x%x\n",
		  subslice_mask, topology_subslices_slice0);

	/*
	 * I915_PARAM_SUBSLICE_MASK returns the value for slice0, we should
	 * match the values for the first slice of the topology.
	 */
	igt_assert_eq(subslice_mask, topology_subslices_slice0);

	free(topo_info);
}

/*
 * Verify that we get same total number of EUs from getparam and topology query.
 */
static void
test_query_topology_matches_eu_total(int fd)
{
	struct drm_i915_query_item item;
	struct drm_i915_query_topology_info *topo_info;
	drm_i915_getparam_t gp;
	int n_eus, n_eus_topology, s;

	gp.param = I915_PARAM_EU_TOTAL;
	gp.value = &n_eus;
	do_ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp);
	igt_debug("n_eus=%i\n", n_eus);

	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_TOPOLOGY_INFO;
	i915_query_items(fd, &item, 1);

	topo_info = calloc(1, item.length);

	item.data_ptr = to_user_pointer(topo_info);
	i915_query_items(fd, &item, 1);

	igt_debug("max_slices=%hu max_subslices=%hu max_eus_per_subslice=%hu\n",
		  topo_info->max_slices, topo_info->max_subslices,
		  topo_info->max_eus_per_subslice);
	igt_debug(" subslice_offset=%hu subslice_stride=%hu\n",
		  topo_info->subslice_offset, topo_info->subslice_stride);
	igt_debug(" eu_offset=%hu eu_stride=%hu\n",
		  topo_info->eu_offset, topo_info->eu_stride);

	n_eus_topology = 0;
	for (s = 0; s < topo_info->max_slices; s++) {
		int ss;

		igt_debug("slice%i: (%s)\n", s,
			  slice_available(topo_info, s) ? "available" : "fused");

		if (!slice_available(topo_info, s))
			continue;

		for (ss = 0; ss < topo_info->max_subslices; ss++) {
			int eu, n_subslice_eus = 0;

			igt_debug("\tsubslice%i: (%s)\n", ss,
				  subslice_available(topo_info, s, ss) ? "available" : "fused");

			if (!subslice_available(topo_info, s, ss))
				continue;

			igt_debug("\t\teu_mask: 0b");
			for (eu = 0; eu < topo_info->max_eus_per_subslice; eu++) {
				uint8_t val = eu_available(topo_info, s, ss,
							   topo_info->max_eus_per_subslice - 1 - eu);
				igt_debug("%hhi", val);
				n_subslice_eus += __builtin_popcount(val);
				n_eus_topology += __builtin_popcount(val);
			}

			igt_debug(" (%i)\n", n_subslice_eus);

			/* Sanity checks. */
			if (n_subslice_eus > 0) {
				igt_assert(slice_available(topo_info, s));
				igt_assert(subslice_available(topo_info, s, ss));
			}
			if (subslice_available(topo_info, s, ss)) {
				igt_assert(slice_available(topo_info, s));
			}
		}
	}

	free(topo_info);

	igt_assert(n_eus_topology == n_eus);
}

/*
 * Verify some numbers on Gens that we know for sure the characteristics from
 * the PCI ids.
 */
static void
test_query_topology_known_pci_ids(int fd, int devid)
{
	const struct intel_device_info *dev_info = intel_get_device_info(devid);
	struct drm_i915_query_item item;
	struct drm_i915_query_topology_info *topo_info;
	int n_slices = 0, n_subslices = 0;
	int s, ss;

	/* The GT size on some Broadwell skus is not defined, skip those. */
	igt_skip_on(dev_info->gt == 0);

	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_TOPOLOGY_INFO;
	i915_query_items(fd, &item, 1);

	topo_info = (struct drm_i915_query_topology_info *) calloc(1, item.length);

	item.data_ptr = to_user_pointer(topo_info);
	i915_query_items(fd, &item, 1);

	for (s = 0; s < topo_info->max_slices; s++) {
		if (slice_available(topo_info, s))
			n_slices++;

		for (ss = 0; ss < topo_info->max_subslices; ss++) {
			if (subslice_available(topo_info, s, ss))
				n_subslices++;
		}
	}

	igt_debug("Platform=%s GT=%u slices=%u subslices=%u\n",
		  dev_info->codename, dev_info->gt, n_slices, n_subslices);

	switch (dev_info->gt) {
	case 1:
		igt_assert_eq(n_slices, 1);
		igt_assert(n_subslices == 1 || n_subslices == 2 || n_subslices == 3);
		break;
	case 2:
		igt_assert_eq(n_slices, 1);
		if (dev_info->is_haswell)
			igt_assert_eq(n_subslices, 2);
		else
			igt_assert_eq(n_subslices, 3);
		break;
	case 3:
		igt_assert_eq(n_slices, 2);
		if (dev_info->is_haswell)
			igt_assert_eq(n_subslices, 2 * 2);
		else
			igt_assert_eq(n_subslices, 2 * 3);
		break;
	case 4:
		igt_assert_eq(n_slices, 3);
		igt_assert_eq(n_subslices, 3 * 3);
		break;
	default:
		igt_assert(false);
	}

	free(topo_info);
}

static bool query_regions_supported(int fd)
{
	struct drm_i915_query_item item = {
		.query_id = DRM_I915_QUERY_MEMORY_REGIONS,
	};

	return __i915_query_items(fd, &item, 1) == 0 && item.length > 0;
}

static bool query_regions_unallocated_supported(int fd)
{
	struct drm_i915_query_memory_regions *regions;
	struct drm_i915_query_item item;
	int i, ret = false;

	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_MEMORY_REGIONS;
	i915_query_items(fd, &item, 1);
	igt_assert(item.length > 0);

	regions = calloc(1, item.length);

	item.data_ptr = to_user_pointer(regions);
	i915_query_items(fd, &item, 1);

	for (i = 0; i < regions->num_regions; i++) {
		struct drm_i915_memory_region_info info = regions->regions[i];

		if (info.unallocated_cpu_visible_size) {
			ret = true;
			break;
		}
	}

	free(regions);
	return ret;
}

static void test_query_regions_garbage_items(int fd)
{
	struct drm_i915_query_memory_regions *regions;
	struct drm_i915_query_item item;
	int i;

	test_query_garbage_items(fd,
				 DRM_I915_QUERY_MEMORY_REGIONS,
				 MIN_REGIONS_ITEM_SIZE,
				 sizeof(struct drm_i915_query_memory_regions));

	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_MEMORY_REGIONS;
	i915_query_items(fd, &item, 1);
	igt_assert(item.length > 0);

	regions = calloc(1, item.length);
	item.data_ptr = to_user_pointer(regions);

	/* Bogus; in-MBZ */
	for (i = 0; i < ARRAY_SIZE(regions->rsvd); i++) {
		regions->rsvd[i] = 0xdeadbeaf;
		i915_query_items(fd, &item, 1);
		igt_assert_eq(item.length, -EINVAL);
		regions->rsvd[i] = 0;
	}

	i915_query_items(fd, &item, 1);
	igt_assert(regions->num_regions);
	igt_assert(item.length > 0);

	/* Bogus; out-MBZ */
	for (i = 0; i < regions->num_regions; i++) {
		struct drm_i915_memory_region_info info = regions->regions[i];
		int j;

		/*
		 * rsvd1[0] : probed_cpu_visible_size
		 * rsvd1[1] : unallocated_cpu_visible_size
		 */
		for (j = 2; j < ARRAY_SIZE(info.rsvd1); j++)
			igt_assert_eq_u64(info.rsvd1[j], 0);
	}

	/* Bogus; kernel is meant to set this */
	regions->num_regions = 1;
	i915_query_items(fd, &item, 1);
	igt_assert_eq(item.length, -EINVAL);
	regions->num_regions = 0;

	free(regions);
}

struct object_handle {
	uint32_t handle;
	struct igt_list_head link;
};

static uint32_t batch_create_size(int fd, uint64_t size)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle;

	handle = gem_create(fd, size);
	gem_write(fd, handle, 0, &bbe, sizeof(bbe));

	return handle;
}

static void upload(int fd, struct igt_list_head *handles, uint32_t num_handles)
{
	struct drm_i915_gem_exec_object2 *exec;
	struct drm_i915_gem_execbuffer2 execbuf = {};
	struct object_handle *iter;
	uint32_t i;

	exec = calloc(num_handles + 1,
		      sizeof(struct drm_i915_gem_exec_object2));

	i = 0;
	igt_list_for_each_entry(iter, handles, link) {
		exec[i].handle = iter->handle;
		exec[i].flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
		i++;
	}

	exec[i].handle = batch_create_size(fd, 4096);
	exec[i].flags = EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

	execbuf.buffers_ptr = to_user_pointer(exec);
	execbuf.buffer_count = num_handles + 1;

	gem_execbuf(fd, &execbuf);
	gem_close(fd, exec[i].handle);
	free(exec);
}

static void test_query_regions_sanity_check(int fd)
{
	struct drm_i915_query_memory_regions *regions;
	struct drm_i915_query_item item;
	bool found_system;
	int i;

	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_MEMORY_REGIONS;
	i915_query_items(fd, &item, 1);
	igt_assert(item.length > 0);

	regions = calloc(1, item.length);

	item.data_ptr = to_user_pointer(regions);
	i915_query_items(fd, &item, 1);

	/* We should always have at least one region */
	igt_assert(regions->num_regions);

	found_system = false;
	for (i = 0; i < regions->num_regions; i++) {
		struct drm_i915_memory_region_info info = regions->regions[i];
		struct drm_i915_gem_memory_class_instance r1 = info.region;
		int j;

		if (r1.memory_class == I915_MEMORY_CLASS_SYSTEM) {
			igt_assert_eq(r1.memory_instance, 0);
			found_system = true;

			igt_assert(info.probed_cpu_visible_size == 0 ||
				   info.probed_cpu_visible_size == info.probed_size);
			igt_assert(info.unallocated_size == info.probed_size);
			igt_assert(info.unallocated_cpu_visible_size == 0 ||
				   info.unallocated_cpu_visible_size ==
				   info.unallocated_size);
		} else {
			igt_assert(info.probed_cpu_visible_size <= info.probed_size);
			igt_assert(info.unallocated_size <= info.probed_size);
			if (info.probed_cpu_visible_size < info.probed_size) {
				igt_assert(info.unallocated_cpu_visible_size <
					   info.unallocated_size);
			} else {
				igt_assert(info.unallocated_cpu_visible_size ==
					   info.unallocated_size);
			}
		}

		igt_assert(r1.memory_class == I915_MEMORY_CLASS_SYSTEM ||
			   r1.memory_class == I915_MEMORY_CLASS_DEVICE);

		for (j = 0; j < regions->num_regions; j++) {
			struct drm_i915_gem_memory_class_instance r2 =
				regions->regions[j].region;

			if (i == j)
				continue;

			/* All probed class:instance pairs must be unique */
			igt_assert(!(r1.memory_class == r2.memory_class &&
				     r1.memory_instance == r2.memory_instance));
		}

		{
			struct igt_list_head handles;
			struct object_handle oh = {};

			IGT_INIT_LIST_HEAD(&handles);

			oh.handle =
				gem_create_with_cpu_access_in_memory_regions
				(fd, 4096,
				 INTEL_MEMORY_REGION_ID(r1.memory_class,
							r1.memory_instance));
			igt_list_add(&oh.link, &handles);
			upload(fd, &handles, 1);

			/*
			 * System wide metrics should be censored if we
			 * lack the correct permissions.
			 */
			igt_fork(child, 1) {
				igt_drop_root();

				memset(regions, 0, item.length);
				i915_query_items(fd, &item, 1);
				info = regions->regions[i];

				igt_assert(info.unallocated_cpu_visible_size ==
					   info.probed_cpu_visible_size);
				igt_assert(info.unallocated_size ==
					   info.probed_size);
			}

			igt_waitchildren();

			memset(regions, 0, item.length);
			i915_query_items(fd, &item, 1);
			info = regions->regions[i];

			if (!info.probed_cpu_visible_size) { /* old kernel */
				igt_assert(info.unallocated_size == info.probed_size);
				igt_assert(info.unallocated_cpu_visible_size == 0);
			} else if (r1.memory_class == I915_MEMORY_CLASS_DEVICE) {
				igt_assert(info.unallocated_cpu_visible_size <
					   info.probed_cpu_visible_size);
				igt_assert(info.unallocated_size <
					   info.probed_size);
			} else {
				igt_assert(info.unallocated_cpu_visible_size ==
					   info.probed_cpu_visible_size);
				igt_assert(info.unallocated_size ==
					   info.probed_size);
			}

			gem_close(fd, oh.handle);
		}
	}

	/* All devices should at least have system memory */
	igt_assert(found_system);

	free(regions);
}

#define rounddown(x, y) (x - (x % y))
#define SZ_64K (1ULL << 16)

static void fill_unallocated(int fd, struct drm_i915_query_item *item, int idx,
			     bool cpu_access)
{
	struct drm_i915_memory_region_info new_info, old_info;
	struct drm_i915_query_memory_regions *regions;
	struct drm_i915_gem_memory_class_instance ci;
	struct object_handle *iter, *tmp;
	struct igt_list_head handles;
	uint32_t num_handles;
	uint64_t rem, total;
	unsigned int seed;
	int id;

	seed = time(NULL);
	srand(seed);

	IGT_INIT_LIST_HEAD(&handles);

	regions = from_user_pointer(item->data_ptr);
	memset(regions, 0, item->length);
	i915_query_items(fd, item, 1);
	new_info = regions->regions[idx];
	ci = new_info.region;

	id = INTEL_MEMORY_REGION_ID(ci.memory_class, ci.memory_instance);

	if (cpu_access)
		rem = new_info.unallocated_cpu_visible_size / 4;
	else
		rem = new_info.unallocated_size / 4;

	rem = rounddown(rem, SZ_64K);
	igt_assert_neq(rem, 0);
	num_handles = 0;
	total = 0;
	do {
		struct object_handle *oh;
		uint64_t size;

		size = rand() % rem;
		size = rounddown(size, SZ_64K);
		size = max_t(uint64_t, size, SZ_64K);

		oh = malloc(sizeof(struct object_handle));
		if (cpu_access)
			oh->handle = gem_create_with_cpu_access_in_memory_regions(fd, size, id);
		else
			oh->handle = gem_create_in_memory_region_list(fd, size, 0, &ci, 1);
		igt_list_add(&oh->link, &handles);

		num_handles++;
		total += size;
		rem -= size;
	} while (rem);

	upload(fd, &handles, num_handles);

	igt_debug("fill completed with seed=%u, cpu_access=%d, idx=%d, total=%"PRIu64"KiB, num_handles=%u\n",
		  seed, cpu_access, idx, total >> 10, num_handles);

	old_info = new_info;
	memset(regions, 0, item->length);
	i915_query_items(fd, item, 1);
	new_info = regions->regions[idx];

	igt_assert_lte_u64(new_info.unallocated_size,
			   new_info.probed_size - total);
	igt_assert_lt_u64(new_info.unallocated_size, old_info.unallocated_size);
	if (new_info.probed_cpu_visible_size ==
	    new_info.probed_size) { /* full BAR */
		igt_assert_eq_u64(new_info.unallocated_cpu_visible_size,
				  new_info.unallocated_size);
	} else if (cpu_access) {
		igt_assert_lt_u64(new_info.unallocated_cpu_visible_size,
				  old_info.unallocated_cpu_visible_size);
		igt_assert_lte_u64(new_info.unallocated_cpu_visible_size,
				   new_info.probed_cpu_visible_size - total);
	}

	igt_list_for_each_entry_safe(iter, tmp, &handles, link) {
		gem_close(fd, iter->handle);
		free(iter);
	}

	igt_drop_caches_set(fd, DROP_ALL);

	old_info = new_info;
	memset(regions, 0, item->length);
	i915_query_items(fd, item, 1);
	new_info = regions->regions[idx];

	igt_assert_lte_u64(old_info.unallocated_size + total,
			   new_info.unallocated_size);
	if (cpu_access)
		igt_assert_lte_u64(old_info.unallocated_cpu_visible_size + total,
				   new_info.unallocated_cpu_visible_size);
}

static void test_query_regions_unallocated(int fd)
{
	struct drm_i915_query_memory_regions *regions;
	struct drm_i915_query_item item;
	int i;

	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_MEMORY_REGIONS;
	i915_query_items(fd, &item, 1);
	igt_assert(item.length > 0);

	regions = calloc(1, item.length);

	item.data_ptr = to_user_pointer(regions);
	i915_query_items(fd, &item, 1);

	igt_assert(regions->num_regions);

	for (i = 0; i < regions->num_regions; i++) {
		struct drm_i915_memory_region_info info = regions->regions[i];
		struct drm_i915_gem_memory_class_instance ci = info.region;

		if (ci.memory_class == I915_MEMORY_CLASS_DEVICE) {
			fill_unallocated(fd, &item, i, true);
			fill_unallocated(fd, &item, i, false);
		}
	}
}

static bool query_engine_info_supported(int fd)
{
	struct drm_i915_query_item item = {
		.query_id = DRM_I915_QUERY_ENGINE_INFO,
	};

	return __i915_query_items(fd, &item, 1) == 0 && item.length > 0;
}

static void engines_invalid(int fd)
{
	struct drm_i915_query_engine_info *engines;
	struct drm_i915_query_item item;
	unsigned int i, len;
	uint8_t *buf;

	/* Flags is MBZ. */
	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_ENGINE_INFO;
	item.flags = 1;
	i915_query_items(fd, &item, 1);
	igt_assert_eq(item.length, -EINVAL);

	/* Length not zero and not greater or equal required size. */
	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_ENGINE_INFO;
	item.length = 1;
	i915_query_items(fd, &item, 1);
	igt_assert_eq(item.length, -EINVAL);

	/* Query correct length. */
	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_ENGINE_INFO;
	i915_query_items(fd, &item, 1);
	igt_assert(item.length >= 0);
	len = item.length;

	engines = malloc(len);
	igt_assert(engines);

	/* Ivalid pointer. */
	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_ENGINE_INFO;
	item.length = len;
	i915_query_items(fd, &item, 1);
	igt_assert_eq(item.length, -EFAULT);

	/* All fields in engines query are MBZ and only filled by the kernel. */

	memset(engines, 0, len);
	engines->num_engines = 1;
	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_ENGINE_INFO;
	item.length = len;
	item.data_ptr = to_user_pointer(engines);
	i915_query_items(fd, &item, 1);
	igt_assert_eq(item.length, -EINVAL);

	memset(engines, 0, len);
	engines->rsvd[0] = 1;
	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_ENGINE_INFO;
	item.length = len;
	item.data_ptr = to_user_pointer(engines);
	i915_query_items(fd, &item, 1);
	igt_assert_eq(item.length, -EINVAL);

	memset(engines, 0, len);
	engines->rsvd[1] = 1;
	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_ENGINE_INFO;
	item.length = len;
	item.data_ptr = to_user_pointer(engines);
	i915_query_items(fd, &item, 1);
	igt_assert_eq(item.length, -EINVAL);

	memset(engines, 0, len);
	engines->rsvd[2] = 1;
	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_ENGINE_INFO;
	item.length = len;
	item.data_ptr = to_user_pointer(engines);
	i915_query_items(fd, &item, 1);
	igt_assert_eq(item.length, -EINVAL);

	free(engines);

	igt_assert(len <= 4096);
	engines = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
		       -1, 0);
	igt_assert(engines != MAP_FAILED);

	/* Check no write past len. */
	memset(engines, 0xa5, 4096);
	memset(engines, 0, len);
	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_ENGINE_INFO;
	item.length = len;
	item.data_ptr = to_user_pointer(engines);
	i915_query_items(fd, &item, 1);
	igt_assert_eq(item.length, len);
	buf = (uint8_t *)engines;
	buf += len;
	for (i = 0; i < 4096 - len; i++, buf++)
		igt_assert_f(*buf == 0xa5,
			     "Garbage %u bytes after buffer! (%x)\n",
			     i, *buf);

	/* PROT_NONE is similar to unmapped area. */
	memset(engines, 0, len);
	igt_assert_eq(mprotect(engines, len, PROT_NONE), 0);
	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_ENGINE_INFO;
	item.length = len;
	item.data_ptr = to_user_pointer(engines);
	i915_query_items(fd, &item, 1);
	igt_assert_eq(item.length, -EFAULT);
	igt_assert_eq(mprotect(engines, len, PROT_WRITE), 0);

	/* Read-only so kernel cannot fill the data back. */
	memset(engines, 0, len);
	igt_assert_eq(mprotect(engines, len, PROT_READ), 0);
	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_ENGINE_INFO;
	item.length = len;
	item.data_ptr = to_user_pointer(engines);
	i915_query_items(fd, &item, 1);
	igt_assert_eq(item.length, -EFAULT);

	munmap(engines, 4096);
}

static bool
has_engine(struct drm_i915_query_engine_info *engines,
	   unsigned class, unsigned instance)
{
	unsigned int i;

	for (i = 0; i < engines->num_engines; i++) {
		struct drm_i915_engine_info *engine =
			(struct drm_i915_engine_info *)&engines->engines[i];

		if (engine->engine.engine_class == class &&
		    engine->engine.engine_instance == instance)
			return true;
	}

	return false;
}

static void engines(int fd)
{
	struct drm_i915_query_engine_info *engines;
	struct drm_i915_query_item item;
	unsigned int len, i;

	engines = malloc(4096);
	igt_assert(engines);

	/* Query required buffer length. */
	memset(engines, 0, 4096);
	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_ENGINE_INFO;
	item.data_ptr = to_user_pointer(engines);
	i915_query_items(fd, &item, 1);
	igt_assert(item.length >= 0);
	igt_assert(item.length <= 4096);
	len = item.length;

	/* Check length larger than required works and reports same length. */
	memset(engines, 0, 4096);
	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_ENGINE_INFO;
	item.length = 4096;
	item.data_ptr = to_user_pointer(engines);
	i915_query_items(fd, &item, 1);
	igt_assert_eq(item.length, len);

	/* Actual query. */
	memset(engines, 0, 4096);
	memset(&item, 0, sizeof(item));
	item.query_id = DRM_I915_QUERY_ENGINE_INFO;
	item.length = len;
	item.data_ptr = to_user_pointer(engines);
	i915_query_items(fd, &item, 1);
	igt_assert_eq(item.length, len);

	/* Every GPU has at least one engine. */
	igt_assert(engines->num_engines > 0);

	/* MBZ fields. */
	igt_assert_eq(engines->rsvd[0], 0);
	igt_assert_eq(engines->rsvd[1], 0);
	igt_assert_eq(engines->rsvd[2], 0);

	/* Confirm the individual engines exist with EXECBUFFER2 */
	for (i = 0; i < engines->num_engines; i++) {
		struct drm_i915_engine_info *engine =
			(struct drm_i915_engine_info *)&engines->engines[i];
		const intel_ctx_t *ctx =
			intel_ctx_create_for_engine(fd, engine->engine.engine_class,
						    engine->engine.engine_instance);
		struct drm_i915_gem_exec_object2 obj = {};
		struct drm_i915_gem_execbuffer2 execbuf = {
			.buffers_ptr = to_user_pointer(&obj),
			.buffer_count = 1,
			.rsvd1 = ctx->id,
		};

		igt_debug("%u: class=%u instance=%u flags=%llx capabilities=%llx\n",
			  i,
			  engine->engine.engine_class,
			  engine->engine.engine_instance,
			  engine->flags,
			  engine->capabilities);
		igt_assert_eq(__gem_execbuf(fd, &execbuf), -ENOENT);

		intel_ctx_destroy(fd, ctx);
	}

	/* Check results match the legacy GET_PARAM (where we can). */
	igt_assert_eq(has_engine(engines, I915_ENGINE_CLASS_RENDER, 0), 1);
	igt_assert_eq(has_engine(engines, I915_ENGINE_CLASS_COPY, 0),
		      gem_has_blt(fd));
	igt_assert_eq(has_engine(engines, I915_ENGINE_CLASS_VIDEO, 0),
		      gem_has_bsd(fd));
	igt_assert_eq(has_engine(engines, I915_ENGINE_CLASS_VIDEO, 1),
		      gem_has_bsd2(fd));
	igt_assert_eq(has_engine(engines, I915_ENGINE_CLASS_VIDEO_ENHANCE, 0),
		      gem_has_vebox(fd));

	free(engines);
}

static void test_query_geometry_subslices(int fd)
{
	const struct intel_execution_engine2 *e;
	struct drm_i915_query_item item = {};
	struct drm_i915_query_topology_info *topo_info;

	/*
	 * Submit an initial request with an invalid engine.  Should return
	 * -EINVAL via item.length.
	 */
	item.query_id = DRM_I915_QUERY_GEOMETRY_SUBSLICES;
	item.flags = ~0;
	i915_query_items(fd, &item, 1);
	igt_assert_eq(item.length, -EINVAL);

	for_each_physical_engine(fd, e) {
		memset(&item, 0, sizeof(item));

		/* Obtain the necessary topology buffer size */
		item.query_id = DRM_I915_QUERY_GEOMETRY_SUBSLICES;
		item.flags =  e->class | (e->instance << 16);
		i915_query_items(fd, &item, 1);

		/* Non-render engines should return -EINVAL */
		if (e->class != I915_ENGINE_CLASS_RENDER) {
			igt_assert_eq(item.length, -EINVAL);
			continue;
		}
		igt_assert(item.length > 0);

		/* Re-submit with a properly allocated buffer */
		topo_info = calloc(1, item.length);
		igt_assert(topo_info);
		item.data_ptr = to_user_pointer(topo_info);
		i915_query_items(fd, &item, 1);

		igt_assert(topo_info->max_subslices > 0);
		igt_assert(topo_info->max_eus_per_subslice > 0);

		igt_assert(topo_info->subslice_offset >=
			   DIV_ROUND_UP(topo_info->max_slices, 8));
		igt_assert(topo_info->eu_offset >=
			   topo_info->subslice_offset + DIV_ROUND_UP(topo_info->max_subslices, 8));

		igt_assert(topo_info->subslice_stride >=
			   DIV_ROUND_UP(topo_info->max_subslices, 8));
		igt_assert(topo_info->eu_stride >=
			   DIV_ROUND_UP(topo_info->max_eus_per_subslice, 8));

		/*
		 * This query is only supported on Xe_HP and beyond, and all
		 * such platforms don't have slices; we should just get a
		 * hardcoded 0x1 for the slice mask.
		 */
		igt_assert_eq(topo_info->max_slices, 1);
		igt_assert_eq(((char*)topo_info->data)[0], 0x1);

		free(topo_info);
	}
}

static const char * const hwconfig_keys[] = {
	[INTEL_HWCONFIG_MAX_SLICES_SUPPORTED] = "Maximum number of Slices",
	[INTEL_HWCONFIG_MAX_DUAL_SUBSLICES_SUPPORTED] = "Maximum number of DSS",
	[INTEL_HWCONFIG_MAX_NUM_EU_PER_DSS] = "Maximum number of EUs per DSS",
	[INTEL_HWCONFIG_NUM_PIXEL_PIPES] = "Pixel Pipes",
	[INTEL_HWCONFIG_DEPRECATED_MAX_NUM_GEOMETRY_PIPES] = "[DEPRECATED] Geometry Pipes",
	[INTEL_HWCONFIG_DEPRECATED_L3_CACHE_SIZE_IN_KB] = "[DEPRECATED] L3 Size (in KB)",
	[INTEL_HWCONFIG_DEPRECATED_L3_BANK_COUNT] = "[DEPRECATED] L3 Bank Count",
	[INTEL_HWCONFIG_L3_CACHE_WAYS_SIZE_IN_BYTES] = "L3 Cache Ways Size (in bytes)",
	[INTEL_HWCONFIG_L3_CACHE_WAYS_PER_SECTOR] = "L3 Cache Ways Per Sector",
	[INTEL_HWCONFIG_MAX_MEMORY_CHANNELS] = "Memory Channels",
	[INTEL_HWCONFIG_MEMORY_TYPE] = "Memory type",
	[INTEL_HWCONFIG_CACHE_TYPES] = "Cache types",
	[INTEL_HWCONFIG_LOCAL_MEMORY_PAGE_SIZES_SUPPORTED] = "Local memory page size",
	[INTEL_HWCONFIG_DEPRECATED_SLM_SIZE_IN_KB] = "[DEPRECATED] SLM Size (in KB)",
	[INTEL_HWCONFIG_NUM_THREADS_PER_EU] = "Num thread per EU",
	[INTEL_HWCONFIG_TOTAL_VS_THREADS] = "Maximum Vertex Shader threads",
	[INTEL_HWCONFIG_TOTAL_GS_THREADS] = "Maximum Geometry Shader threads",
	[INTEL_HWCONFIG_TOTAL_HS_THREADS] = "Maximum Hull Shader threads",
	[INTEL_HWCONFIG_TOTAL_DS_THREADS] = "Maximum Domain Shader threads",
	[INTEL_HWCONFIG_TOTAL_VS_THREADS_POCS] = "Maximum Vertex Shader Threads for POCS",
	[INTEL_HWCONFIG_TOTAL_PS_THREADS] = "Maximum Pixel Shader Threads",
	[INTEL_HWCONFIG_DEPRECATED_MAX_FILL_RATE] = "[DEPRECATED] Maximum pixel rate for Fill",
	[INTEL_HWCONFIG_MAX_RCS] = "MaxRCS",
	[INTEL_HWCONFIG_MAX_CCS] = "MaxCCS",
	[INTEL_HWCONFIG_MAX_VCS] = "MaxVCS",
	[INTEL_HWCONFIG_MAX_VECS] = "MaxVECS",
	[INTEL_HWCONFIG_MAX_COPY_CS] = "MaxCopyCS",
	[INTEL_HWCONFIG_DEPRECATED_URB_SIZE_IN_KB] = "[DEPRECATED] URB Size (in KB)",
	[INTEL_HWCONFIG_MIN_VS_URB_ENTRIES] = "The minimum number of VS URB entries.",
	[INTEL_HWCONFIG_MAX_VS_URB_ENTRIES] = "The maximum number of VS URB entries.",
	[INTEL_HWCONFIG_MIN_PCS_URB_ENTRIES] = "The minimum number of PCS URB entries",
	[INTEL_HWCONFIG_MAX_PCS_URB_ENTRIES] = "The maximum number of PCS URB entries",
	[INTEL_HWCONFIG_MIN_HS_URB_ENTRIES] = "The minimum number of HS URB entries",
	[INTEL_HWCONFIG_MAX_HS_URB_ENTRIES] = "The maximum number of HS URB entries",
	[INTEL_HWCONFIG_MIN_GS_URB_ENTRIES] = "The minimum number of GS URB entries",
	[INTEL_HWCONFIG_MAX_GS_URB_ENTRIES] = "The maximum number of GS URB entries",
	[INTEL_HWCONFIG_MIN_DS_URB_ENTRIES] = "The minimum number of DS URB Entries",
	[INTEL_HWCONFIG_MAX_DS_URB_ENTRIES] = "The maximum number of DS URB Entries",
	[INTEL_HWCONFIG_PUSH_CONSTANT_URB_RESERVED_SIZE] = "Push Constant URB Reserved Size (in bytes)",
	[INTEL_HWCONFIG_POCS_PUSH_CONSTANT_URB_RESERVED_SIZE] = "POCS Push Constant URB Reserved Size (in bytes)",
	[INTEL_HWCONFIG_URB_REGION_ALIGNMENT_SIZE_IN_BYTES] = "URB Region Alignment Size (in bytes)",
	[INTEL_HWCONFIG_URB_ALLOCATION_SIZE_UNITS_IN_BYTES] = "URB Allocation Size Units (in bytes)",
	[INTEL_HWCONFIG_MAX_URB_SIZE_CCS_IN_BYTES] = "Max URB Size CCS (in bytes)",
	[INTEL_HWCONFIG_VS_MIN_DEREF_BLOCK_SIZE_HANDLE_COUNT] = "VS Min Deref BlockSize Handle Count",
	[INTEL_HWCONFIG_DS_MIN_DEREF_BLOCK_SIZE_HANDLE_COUNT] = "DS Min Deref Block Size Handle Count",
	[INTEL_HWCONFIG_NUM_RT_STACKS_PER_DSS] = "Num RT Stacks Per DSS",
	[INTEL_HWCONFIG_MAX_URB_STARTING_ADDRESS] = "Max URB Starting Address",
	[INTEL_HWCONFIG_MIN_CS_URB_ENTRIES] = "Min CS URB Entries",
	[INTEL_HWCONFIG_MAX_CS_URB_ENTRIES] = "Max CS URB Entries",
	[INTEL_HWCONFIG_L3_ALLOC_PER_BANK_URB] = "L3 Alloc Per Bank - URB",
	[INTEL_HWCONFIG_L3_ALLOC_PER_BANK_REST] = "L3 Alloc Per Bank - Rest",
	[INTEL_HWCONFIG_L3_ALLOC_PER_BANK_DC] = "L3 Alloc Per Bank - DC",
	[INTEL_HWCONFIG_L3_ALLOC_PER_BANK_RO] = "L3 Alloc Per Bank - RO",
	[INTEL_HWCONFIG_L3_ALLOC_PER_BANK_Z] = "L3 Alloc Per Bank - Z",
	[INTEL_HWCONFIG_L3_ALLOC_PER_BANK_COLOR] = "L3 Alloc Per Bank - Color",
	[INTEL_HWCONFIG_L3_ALLOC_PER_BANK_UNIFIED_TILE_CACHE] = "L3 Alloc Per Bank - Unified Tile Cache",
	[INTEL_HWCONFIG_L3_ALLOC_PER_BANK_COMMAND_BUFFER] = "L3 Alloc Per Bank - Command Buffer",
	[INTEL_HWCONFIG_L3_ALLOC_PER_BANK_RW] = "L3 Alloc Per Bank - RW",
	[INTEL_HWCONFIG_MAX_NUM_L3_CONFIGS] = "Num L3 Configs",
	[INTEL_HWCONFIG_BINDLESS_SURFACE_OFFSET_BIT_COUNT] = "Bindless Surface Offset Bit Count",
	[INTEL_HWCONFIG_RESERVED_CCS_WAYS] = "Reserved CCS ways",
	[INTEL_HWCONFIG_CSR_SIZE_IN_MB] = "CSR Size (in MB)",
	[INTEL_HWCONFIG_GEOMETRY_PIPES_PER_SLICE] = "Geometry pipes per slice",
	[INTEL_HWCONFIG_L3_BANK_SIZE_IN_KB] = "L3 bank size (in KB)",
	[INTEL_HWCONFIG_SLM_SIZE_PER_DSS] = "SLM size per DSS",
	[INTEL_HWCONFIG_MAX_PIXEL_FILL_RATE_PER_SLICE] = "Max pixel fill rate per slice",
	[INTEL_HWCONFIG_MAX_PIXEL_FILL_RATE_PER_DSS] = "Max pixel fill rate per DSS",
	[INTEL_HWCONFIG_URB_SIZE_PER_SLICE_IN_KB] = "URB size per slice (in KB)",
	[INTEL_HWCONFIG_URB_SIZE_PER_L3_BANK_COUNT_IN_KB] = "URB size per L3 bank count (in KB)",
	[INTEL_HWCONFIG_MAX_SUBSLICE] = "Max subslices",
	[INTEL_HWCONFIG_MAX_EU_PER_SUBSLICE] = "Max EUs per subslice",
	[INTEL_HWCONFIG_RAMBO_L3_BANK_SIZE_IN_KB] = "RAMBO L3 bank size (in KB)",
	[INTEL_HWCONFIG_SLM_SIZE_PER_SS_IN_KB] = "SLM size per SS (in KB)",
	[INTEL_HWCONFIG_NUM_HBM_STACKS_PER_TILE] = "Num HBM Stacks Per Tile",
	[INTEL_HWCONFIG_NUM_CHANNELS_PER_HBM_STACK] = "Num Channels Per HBM Stack",
	[INTEL_HWCONFIG_HBM_CHANNEL_WIDTH_IN_BYTES] = "HBM Channel Width (in bytes)",
	[INTEL_HWCONFIG_MIN_TASK_URB_ENTRIES] = "Min Task URB Entries",
	[INTEL_HWCONFIG_MAX_TASK_URB_ENTRIES] = "Max Task URB Entries",
	[INTEL_HWCONFIG_MIN_MESH_URB_ENTRIES] = "Min Mesh URB Entries",
	[INTEL_HWCONFIG_MAX_MESH_URB_ENTRIES] = "Max Mesh URB Entries",
};

static const char * const hwconfig_memtypes[] = {
	[INTEL_HWCONFIG_MEMORY_TYPE_LPDDR4] = "LPDDR4",
	[INTEL_HWCONFIG_MEMORY_TYPE_LPDDR5] = "LPDDR5",
	[INTEL_HWCONFIG_MEMORY_TYPE_HBM2] = "HBM2",
	[INTEL_HWCONFIG_MEMORY_TYPE_HBM2e] = "HBM2e",
	[INTEL_HWCONFIG_MEMORY_TYPE_GDDR6] = "GDDR6",
};

static const char * const hwconfig_cachetypes[] = {
	[INTEL_HWCONFIG_CACHE_TYPE_L3] = "L3",
	[INTEL_HWCONFIG_CACHE_TYPE_LLC] = "LLC",
	[INTEL_HWCONFIG_CACHE_TYPE_EDRAM] = "EDRAM",
};

static void query_parse_and_validate_hwconfig_table(int i915)
{
	struct drm_i915_query_item item = {
		.query_id = DRM_I915_QUERY_HWCONFIG_BLOB,
	};
	uint32_t *data, value;
	int i = 0;
	int len, j, max_words, table_size;

	igt_assert(ARRAY_SIZE(hwconfig_keys) == __INTEL_HWCONFIG_KEY_LIMIT);
	igt_assert(ARRAY_SIZE(hwconfig_memtypes) == __INTEL_HWCONFIG_MEMORY_TYPE_LIMIT);
	igt_assert(ARRAY_SIZE(hwconfig_cachetypes) == __INTEL_HWCONFIG_CACHE_TYPE_LIMIT);

	i915_query_items(i915, &item, 1);
	table_size = item.length;
	igt_require(table_size > 0);

	data = malloc(table_size);
	igt_assert(data);
	memset(data, 0, table_size);
	item.data_ptr = to_user_pointer(data);

	i915_query_items(i915, &item, 1);
	igt_assert(item.length == table_size);
	igt_info("Table size = %d bytes\n", table_size);
	igt_assert(table_size > 0);

	/* HWConfig table is a list of KLV sets */
	max_words = table_size / sizeof(uint32_t);
	igt_assert(max_words * sizeof(uint32_t) == table_size);
	while (i < max_words) {
		/* Attribute ID zero is invalid */
		igt_assert(data[i] > 0);
		igt_assert(data[i] < __INTEL_HWCONFIG_KEY_LIMIT);

		len = data[i + 1];
		igt_assert(len > 0);
		igt_assert((i + 2 + len) <= max_words);

		igt_info("[%2d] %s: ", data[i], hwconfig_keys[data[i]]);

		value = data[i + 2];
		switch (data[i]) {
		case INTEL_HWCONFIG_MEMORY_TYPE:
			igt_assert(len == 1);
			igt_assert(value < __INTEL_HWCONFIG_MEMORY_TYPE_LIMIT);
			igt_info("%s\n", hwconfig_memtypes[value]);
			break;

		case INTEL_HWCONFIG_CACHE_TYPES:
			igt_assert(len == 1);

			if (!value)
				igt_info("-\n");

			for (j = 0; j < __INTEL_HWCONFIG_CACHE_TYPE_LIMIT; j++) {
				if (value & BIT(j)) {
					value &= ~BIT(j);
					igt_info("%s%s", hwconfig_cachetypes[j], value ? ", " : "\n");
				}
			}

			igt_assert(!value);
			break;

		default:
			for (j = i + 2; j < i + 1 + len; j++)
				igt_info("%d, ", data[j]);
			igt_info("%d\n", data[j]);
		}

		/* Advance to next key */
		i += 2 + len;
	}

	free(data);
}

igt_main
{
	int fd = -1;
	int devid;

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		igt_require(has_query_supports(fd));
		devid = intel_get_drm_devid(fd);
	}

	igt_describe("Test response to an invalid query call");
	igt_subtest("query-garbage")
		test_query_garbage(fd);

	igt_describe("Test response to invalid DRM_I915_QUERY_TOPOLOGY_INFO query");
	igt_subtest("query-topology-garbage-items") {
		igt_require(query_topology_supported(fd));
		test_query_topology_garbage_items(fd);
	}

	igt_describe("Guardband test for DRM_I915_QUERY_TOPOLOGY_INFO query");
	igt_subtest("query-topology-kernel-writes") {
		igt_require(query_topology_supported(fd));
		test_query_topology_kernel_writes(fd);
	}

	igt_describe("Verify DRM_I915_QUERY_TOPOLOGY_INFO query fails when it is not supported");
	igt_subtest("query-topology-unsupported") {
		igt_require(!query_topology_supported(fd));
		test_query_topology_unsupported(fd);
	}

	igt_describe("Compare new DRM_I915_QUERY_TOPOLOGY_INFO query with legacy (sub)slice getparams");
	igt_subtest("query-topology-coherent-slice-mask") {
		igt_require(query_topology_supported(fd));
		test_query_topology_coherent_slice_mask(fd);
	}

	igt_describe("More compare new DRM_I915_QUERY_TOPOLOGY_INFO query with legacy (sub)slice getparams");
	igt_subtest("query-topology-matches-eu-total") {
		igt_require(query_topology_supported(fd));
		test_query_topology_matches_eu_total(fd);
	}

	igt_describe("Verify DRM_I915_QUERY_TOPOLOGY_INFO query against hardcoded known values for certain platforms");
	igt_subtest("query-topology-known-pci-ids") {
		igt_require(query_topology_supported(fd));
		igt_require(IS_HASWELL(devid) || IS_BROADWELL(devid) ||
			    IS_SKYLAKE(devid) || IS_KABYLAKE(devid) ||
			    IS_COFFEELAKE(devid));
		test_query_topology_known_pci_ids(fd, devid);
	}

	igt_describe("Test DRM_I915_QUERY_GEOMETRY_SUBSLICES query");
	igt_subtest("test-query-geometry-subslices") {
		igt_require(query_geometry_subslices_supported(fd));
		test_query_geometry_subslices(fd);
	}

	igt_describe("Dodgy returned data tests for DRM_I915_QUERY_MEMORY_REGIONS");
	igt_subtest("query-regions-garbage-items") {
		igt_require(query_regions_supported(fd));
		test_query_regions_garbage_items(fd);
	}

	igt_describe("Basic tests for DRM_I915_QUERY_MEMORY_REGIONS");
	igt_subtest("query-regions-sanity-check") {
		igt_require(query_regions_supported(fd));
		test_query_regions_sanity_check(fd);
	}

	igt_describe("Sanity check the region unallocated tracking");
	igt_subtest("query-regions-unallocated") {
		igt_require(query_regions_supported(fd));
		igt_require(query_regions_unallocated_supported(fd));
		test_query_regions_unallocated(fd);
	}

	igt_subtest_group {
		igt_fixture {
			igt_require(query_engine_info_supported(fd));
		}

		igt_describe("Negative tests for DRM_I915_QUERY_ENGINE_INFO");
		igt_subtest("engine-info-invalid")
			engines_invalid(fd);

		igt_describe("Positive tests for DRM_I915_QUERY_ENGINE_INFO");
		igt_subtest("engine-info")
			engines(fd);
	}

	igt_describe("Test DRM_I915_QUERY_HWCONFIG_BLOB query");
	igt_subtest("hwconfig_table")
		query_parse_and_validate_hwconfig_table(fd);

	igt_fixture {
		close(fd);
	}
}
