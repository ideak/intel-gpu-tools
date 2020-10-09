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
 *
 */

#include "igt_core.h"
#include "igt_device_scan.h"
#include "igt_list.h"

#include <dirent.h>
#include <fcntl.h>
#include <glib.h>
#include <libudev.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <sys/types.h>

/**
 * SECTION:igt_device_scan
 * @short_description: Device scanning and selection
 * @title: Device selection
 * @include: igt.h
 *
 * # Device scanning
 *
 * Device scanning iterates over DRM subsystem using udev library
 * to acquire DRM devices.
 * For each DRM device we also get and store its parent to allow device
 * selection happen in a more contextual way.
 *
 * Parent devices are bus devices (like PCI, platform, etc.) and contain a lot
 * of extra data on top of the DRM device itself.
 *
 * # Filters
 *
 * Device selection can be done using filters that are using the data collected
 * udev + some syntactic sugar.
 *
 * Direct device selection filter uses sysfs path to find the device:
 *
 * |[<!-- language="plain" -->
 * sys:/sys/path/to/device/or/parent
 * ]|
 *
 * Examples:
 * |[<!-- language="plain" -->
 * - sys:/sys/devices/pci0000:00/0000:00:02.0/drm/card0
 * - sys:/sys/devices/pci0000:00/0000:00:02.0
 * - sys:/sys/devices/platform/vgem
 * ]|
 *
 * The alternative is to use other implemented filters:
 *
 * - drm: get drm /dev/dri/... device directly
 *
 *   |[<!-- language="plain" -->
 *   drm:/dev/dri/...
 *   ]|
 *
 *   Loading drivers in different order can cause different ordering of
 *   /dev/dri/card nodes which be problematic for reliable and reproducible
 *   device selection, e.g. in automated execution setting. In such scenarios
 *   please consider using sys, pci or platform filters instead.
 *
 * - pci: select device using PCI vendor and device properties
 *   |[<!-- language="plain" -->
 *   pci:[vendor=%04x/name][,device=%04x][,card=%d]
 *   ]|
 *
 *   Filter allows device selection using vendor (hex or name), device id
 *   (hex) and nth-card from all matches. For example if there are 4 PCI cards
 *   installed (let two cards have 1234 and other two 1235 device id, all of
 *   them of vendor Intel) you can select one using:
 *
 *   |[<!-- language="plain" -->
 *   pci:vendor=Intel,device=1234,card=0
 *   ]|
 *
 *   or
 *
 *   |[<!-- language="plain" -->
 *   pci:vendor=8086,device=1234,card=0
 *   ]|
 *
 *   This takes first device with 1234 id for Intel vendor (8086).
 *
 *   |[<!-- language="plain" -->
 *   pci:vendor=Intel,device=1234,card=1
 *   ]|
 *
 *   or
 *
 *   |[<!-- language="plain" -->
 *   pci:vendor=8086,device=1234,card=1
 *   ]|
 *
 *   It selects the second one.
 *
 *   As order the on PCI bus doesn't change (unless you'll add new device or
 *   reorder existing one) device selection using this filter will always
 *   return you same device regardless the order of enumeration.
 *
 *   Simple syntactic sugar over using the sysfs paths.
 */

#ifdef DEBUG_DEVICE_SCAN
#define DBG(...) \
{ \
	struct timeval tm; \
	gettimeofday(&tm, NULL); \
	printf("%10ld.%03ld: ", tm.tv_sec, tm.tv_usec); \
	printf(__VA_ARGS__); \
}

#else
#define DBG(...) {}
#endif

static inline bool strequal(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return false;

	return strcmp(a, b) == 0;
}

struct igt_device {
	/* Filled for drm devices */
	struct igt_device *parent;

	/* Point to vendor spec if can be found */

	/* Properties / sysattrs rewriten from udev lists */
	GHashTable *props_ht;
	GHashTable *attrs_ht;

	/* Most usable variables from udev device */
	char *subsystem;
	char *syspath;
	char *devnode;

	/* /dev/dri/... paths */
	char *drm_card;
	char *drm_render;

	/* For pci subsystem */
	char *vendor;
	char *device;
	char *pci_slot_name;

	struct igt_list_head link;
};

/* Scanned devices */
static struct {
	struct igt_list_head all;
	struct igt_list_head filtered;
	bool devs_scanned;
} igt_devs;

static struct {
	const char *name;
	const char *vendor_id;
} pci_vendor_mapping[] = {
	{ "intel", "8086" },
	{ "amd", "1002" },
	{ NULL, },
};

static const char *get_pci_vendor_id_by_name(const char *name)
{
	for (typeof(*pci_vendor_mapping) *vm = pci_vendor_mapping; vm->name; vm++)
	{
		if (!strcasecmp(name, vm->name))
			return vm->vendor_id;
	}

	return NULL;
}

/* Reading sysattr values can take time (even seconds),
 * we want to avoid reading such keys.
 */
static bool is_on_blacklist(const char *what)
{
	static const char *keys[] = { "config", "modalias", "modes",
				      "resource",
				      "resource0", "resource1", "resource2",
				      "resource3", "resource4", "resource5",
				      "resource0_wc", "resource1_wc", "resource2_wc",
				      "resource3_wc", "resource4_wc", "resource5_wc",
				      "driver",
				      "uevent", NULL};
	const char *key;
	int i = 0;

	if (what == NULL)
		return false;

	while ((key = keys[i++])) {
		if (strcmp(key, what) == 0)
			return true;
	}

	return false;

}

static struct igt_device *igt_device_new(void)
{
	struct igt_device *dev;

	dev = calloc(1, sizeof(struct igt_device));
	if (!dev)
		return NULL;

	dev->attrs_ht = g_hash_table_new_full(g_str_hash, g_str_equal,
					      free, free);
	dev->props_ht = g_hash_table_new_full(g_str_hash, g_str_equal,
					      free, free);

	if (dev->attrs_ht && dev->props_ht)
		return dev;

	free(dev);

	return NULL;
}

static void igt_device_add_prop(struct igt_device *dev,
				const char *key, const char *value)
{
	if (!key || !value)
		return;

	g_hash_table_insert(dev->props_ht, strdup(key), strdup(value));
}

static void igt_device_add_attr(struct igt_device *dev,
				const char *key, const char *value)
{
	const char *v = value;

	if (!key)
		return;

	/* It's possible we have symlink at key filename, but udev
	 * library resolves only few of them
	 */
	if (!v) {
		struct stat st;
		char path[PATH_MAX];
		char linkto[PATH_MAX];
		int len;

		snprintf(path, sizeof(path), "%s/%s", dev->syspath, key);
		if (lstat(path, &st) != 0)
			return;

		len = readlink(path, linkto, sizeof(linkto));
		if (len <= 0 || len == (ssize_t) sizeof(linkto))
			return;
		linkto[len] = '\0';
		v = strrchr(linkto, '/');
		if (v == NULL)
			return;
		v++;
	}

	g_hash_table_insert(dev->attrs_ht, strdup(key), strdup(v));
}

/* Iterate over udev properties list and rewrite it to igt_device properties
 * hash table for instant access.
 */
static void get_props(struct udev_device *dev, struct igt_device *idev)
{
	struct udev_list_entry *entry;

	entry = udev_device_get_properties_list_entry(dev);
	while (entry) {
		const char *name = udev_list_entry_get_name(entry);
		const char *value = udev_list_entry_get_value(entry);

		igt_device_add_prop(idev, name, value);
		entry = udev_list_entry_get_next(entry);
		DBG("prop: %s, val: %s\n", name, value);
	}
}

/* Same as get_props(), but rewrites sysattrs. Resolves symbolic links
 * not handled by udev get_sysattr_value().
 * Function skips sysattrs from blacklist ht (acquiring some values can take
 * seconds).
 */
static void get_attrs(struct udev_device *dev, struct igt_device *idev)
{
	struct udev_list_entry *entry;

	entry = udev_device_get_sysattr_list_entry(dev);
	while (entry) {
		const char *key = udev_list_entry_get_name(entry);
		const char *value;

		if (is_on_blacklist(key)) {
			entry = udev_list_entry_get_next(entry);
			continue;
		}

		value = udev_device_get_sysattr_value(dev, key);
		igt_device_add_attr(idev, key, value);
		entry = udev_list_entry_get_next(entry);
		DBG("attr: %s, val: %s\n", key, value);
	}
}

#define get_prop(dev, prop) ((char *) g_hash_table_lookup(dev->props_ht, prop))
#define get_attr(dev, attr) ((char *) g_hash_table_lookup(dev->attrs_ht, attr))
#define get_prop_subsystem(dev) get_prop(dev, "SUBSYSTEM")
#define is_drm_subsystem(dev)  (strequal(get_prop_subsystem(dev), "drm"))
#define is_pci_subsystem(dev)  (strequal(get_prop_subsystem(dev), "pci"))

/*
 * Get PCI_SLOT_NAME property, it should be in format of
 * xxxx:yy:zz.z
 */
static void set_pci_slot_name(struct igt_device *dev)
{
	const char *pci_slot_name = get_prop(dev, "PCI_SLOT_NAME");

	if (!pci_slot_name || (strlen(pci_slot_name) != PCI_SLOT_NAME_SIZE))
		return;
	dev->pci_slot_name = strdup(pci_slot_name);
}

/*
 * Gets PCI_ID property, splits to xxxx:yyyy and stores
 * xxxx to dev->vendor and yyyy to dev->device for
 * faster access.
 */
static void set_vendor_device(struct igt_device *dev)
{
	const char *pci_id = get_prop(dev, "PCI_ID");

	if (!pci_id || strlen(pci_id) != 9)
		return;
	dev->vendor = strndup(pci_id, 4);
	dev->device = strndup(pci_id + 5, 4);
}

/* Initialize lists for keeping scanned devices */
static bool prepare_scan(void)
{
	IGT_INIT_LIST_HEAD(&igt_devs.all);
	IGT_INIT_LIST_HEAD(&igt_devs.filtered);

	return true;
}

static char* strdup_nullsafe(const char* str)
{
	if (str == NULL)
		return NULL;

	return strdup(str);
}

/* Create new igt_device from udev device.
 * Fills structure with most usable udev device variables, properties
 * and sysattrs.
 */
static struct igt_device *igt_device_new_from_udev(struct udev_device *dev)
{
	struct igt_device *idev = igt_device_new();

	igt_assert(idev);
	idev->syspath = strdup_nullsafe(udev_device_get_syspath(dev));
	idev->subsystem = strdup_nullsafe(udev_device_get_subsystem(dev));
	idev->devnode = strdup_nullsafe(udev_device_get_devnode(dev));

	if (idev->devnode && strstr(idev->devnode, "/dev/dri/card"))
		idev->drm_card = strdup(idev->devnode);
	else if (idev->devnode && strstr(idev->devnode, "/dev/dri/render"))
		idev->drm_render = strdup(idev->devnode);

	get_props(dev, idev);
	get_attrs(dev, idev);

	return idev;
}

/* Iterate over all igt_devices array and find one matched to
 * subsystem and syspath.
 */
static struct igt_device *igt_device_find(const char *subsystem,
					  const char *syspath)
{
	struct igt_device *dev;

	igt_list_for_each_entry(dev, &igt_devs.all, link)
	{
		if (strcmp(dev->subsystem, subsystem) == 0 &&
			strcmp(dev->syspath, syspath) == 0)
			return dev;
	}

	return NULL;
}

static void
__copy_dev_to_card(struct igt_device *dev, struct igt_device_card *card)
{
	if (dev->subsystem != NULL)
		strncpy(card->subsystem, dev->subsystem,
			sizeof(card->subsystem) - 1);

	if (dev->drm_card != NULL)
		strncpy(card->card, dev->drm_card, sizeof(card->card) - 1);

	if (dev->drm_render != NULL)
		strncpy(card->render, dev->drm_render,
			sizeof(card->render) - 1);

	if (dev->pci_slot_name != NULL)
		strncpy(card->pci_slot_name, dev->pci_slot_name,
			PCI_SLOT_NAME_SIZE + 1);
}

/*
 * Iterate over all igt_devices array and find first discrete card.
 * card->pci_slot_name will be updated only if a discrete card is present.
 */
bool igt_device_find_first_i915_discrete_card(struct igt_device_card *card)
{
	struct igt_device *dev;

	memset(card, 0, sizeof(*card));

	igt_list_for_each_entry(dev, &igt_devs.all, link) {

		if (!is_pci_subsystem(dev))
			continue;

		if ((strncmp(dev->pci_slot_name, INTEGRATED_I915_GPU_PCI_ID, PCI_SLOT_NAME_SIZE)) != 0) {
			__copy_dev_to_card(dev, card);
			return true;
		}
	}

	return false;
}

static struct igt_device *igt_device_from_syspath(const char *syspath)
{
	struct igt_device *dev;

	igt_list_for_each_entry(dev, &igt_devs.all, link)
	{
		if (strcmp(dev->syspath, syspath) == 0)
			return dev;
	}

	return NULL;
}

/* For each drm igt_device add or update its parent igt_device to the array.
 * As card/render drm devices mostly have same parent (vkms is an exception)
 * link to it and update corresponding drm_card / drm_render fields.
 */
static void update_or_add_parent(struct udev_device *dev,
				 struct igt_device *idev)
{
	struct udev_device *parent_dev;
	struct igt_device *parent_idev;
	const char *subsystem, *syspath, *devname;

	parent_dev = udev_device_get_parent(dev);
	igt_assert(parent_dev);

	subsystem = udev_device_get_subsystem(parent_dev);
	syspath = udev_device_get_syspath(parent_dev);

	parent_idev = igt_device_find(subsystem, syspath);
	if (!parent_idev) {
		parent_idev = igt_device_new_from_udev(parent_dev);
		if (is_pci_subsystem(parent_idev)) {
			set_vendor_device(parent_idev);
			set_pci_slot_name(parent_idev);
		}
		igt_list_add_tail(&parent_idev->link, &igt_devs.all);
	}
	devname = udev_device_get_devnode(dev);
	if (devname != NULL && strstr(devname, "/dev/dri/card"))
		parent_idev->drm_card = strdup(devname);
	else if (devname != NULL && strstr(devname, "/dev/dri/render"))
		parent_idev->drm_render = strdup(devname);

	idev->parent = parent_idev;
}

static struct igt_device *duplicate_device(struct igt_device *dev) {
	struct igt_device *dup = malloc(sizeof(*dup));
	memcpy(dup, dev, sizeof(*dev));
	dup->link.prev = NULL;
	dup->link.next = NULL;
	return dup;
}

static gint devs_compare(const void *a, const void *b)
{
	struct igt_device *dev1, *dev2;
	int ret;

	dev1 = *(struct igt_device **) a;
	dev2 = *(struct igt_device **) b;
	ret = strcmp(dev1->subsystem, dev2->subsystem);
	if (ret)
		return ret;

	return strcmp(dev1->syspath, dev2->syspath);
}

static void sort_all_devices(void)
{
	struct igt_device *dev, *tmp;
	int len = igt_list_length(&igt_devs.all);
	struct igt_device **devs = malloc(len * sizeof(*dev));

	int i = 0;
	igt_list_for_each_entry_safe(dev, tmp, &igt_devs.all, link) {
		devs[i] = dev;
		igt_assert(i++ < len);
		igt_list_del(&dev->link);
	}

	qsort(devs, len, sizeof(*devs), devs_compare);

	for (i = 0; i < len; ++i) {
		igt_list_add_tail(&devs[i]->link, &igt_devs.all);
	}

	free(devs);
}

/* Core scanning function.
 *
 * All scanned devices are kept inside igt_devs.all pointer array.
 * Each added device is igt_device structure, which contrary to udev device
 * has properties / sysattrs stored inside hash table instead of list.
 *
 * Function iterates over devices on 'drm' subsystem. For each drm device
 * its parent is taken (bus device) and stored inside same array.
 * Function sorts all found devices to keep same order of bus devices
 * for providing predictable search.
 */
static void scan_drm_devices(void)
{
	struct udev *udev;
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *dev_list_entry;
	struct igt_device *dev;
	int ret;

	udev = udev_new();
	igt_assert(udev);

	enumerate = udev_enumerate_new(udev);
	igt_assert(enumerate);

	DBG("Scanning drm subsystem\n");
	ret = udev_enumerate_add_match_subsystem(enumerate, "drm");
	igt_assert(!ret);

	ret = udev_enumerate_add_match_property(enumerate, "DEVNAME", "/dev/dri/*");
	igt_assert(!ret);

	ret = udev_enumerate_scan_devices(enumerate);
	igt_assert(!ret);

	devices = udev_enumerate_get_list_entry(enumerate);
	if (!devices)
		return;

	udev_list_entry_foreach(dev_list_entry, devices) {
		const char *path;
		struct udev_device *udev_dev;
		struct igt_device *idev;

		path = udev_list_entry_get_name(dev_list_entry);
		udev_dev = udev_device_new_from_syspath(udev, path);
		idev = igt_device_new_from_udev(udev_dev);
		update_or_add_parent(udev_dev, idev);
		igt_list_add_tail(&idev->link, &igt_devs.all);

		udev_device_unref(udev_dev);
	}
	udev_enumerate_unref(enumerate);
	udev_unref(udev);

	sort_all_devices();

	igt_list_for_each_entry(dev, &igt_devs.all, link) {
		struct igt_device *dev_dup = duplicate_device(dev);
		igt_list_add_tail(&dev_dup->link, &igt_devs.filtered);
	}
}

static void igt_device_free(struct igt_device *dev)
{
	free(dev->devnode);
	free(dev->subsystem);
	free(dev->syspath);
	free(dev->drm_card);
	free(dev->drm_render);
	free(dev->vendor);
	free(dev->device);
	free(dev->pci_slot_name);
	g_hash_table_destroy(dev->attrs_ht);
	g_hash_table_destroy(dev->props_ht);
}

void igt_devices_free(void)
{
	struct igt_device *dev, *tmp;

	igt_list_for_each_entry_safe(dev, tmp, &igt_devs.all, link) {
		igt_device_free(dev);
		free(dev);
	}
}

/**
 * igt_devices_scan
 * @force: enforce scanning devices
 *
 * Function scans udev in search of gpu devices. For first run it can be
 * called with @force = false. If something changes during the the test
 * or test does some module loading (new drm devices occurs during execution)
 * function must be called again with @force = true to refresh device array.
 */
void igt_devices_scan(bool force)
{
	if (force && igt_devs.devs_scanned) {
		struct igt_device *dev, *tmp;

		igt_list_for_each_entry_safe(dev, tmp, &igt_devs.filtered,
					     link) {
			igt_list_del(&dev->link);
			free(dev);
		}
		igt_list_for_each_entry_safe(dev, tmp, &igt_devs.all, link) {
			igt_list_del(&dev->link);
			igt_device_free(dev);
			free(dev);
		}

		igt_devs.devs_scanned = false;
	}

	if (igt_devs.devs_scanned)
		return;

	prepare_scan();
	scan_drm_devices();

	igt_devs.devs_scanned = true;
}

static inline void _pr_simple(const char *k, const char *v)
{
	printf("    %-16s: %s\n", k, v);
}

static inline void _pr_simple2(const char *k, const char *v1, const char *v2)
{
	printf("    %-16s: %s:%s\n", k, v1, v2);
}

static void igt_devs_print_simple(struct igt_list_head *view)
{
	struct igt_device *dev;

	if (!view)
		return;

	if (igt_list_empty(view)) {
		printf("No GPU devices found\n");
		return;
	}

	igt_list_for_each_entry(dev, view, link) {
		printf("sys:%s\n", dev->syspath);
		if (dev->subsystem)
			_pr_simple("subsystem", dev->subsystem);
		if (dev->drm_card)
			_pr_simple("drm card", dev->drm_card);
		if (dev->drm_render)
			_pr_simple("drm render", dev->drm_render);
		if (is_drm_subsystem(dev)) {
			_pr_simple2("parent", "sys",
				   dev->parent->syspath);
		} else {
			if (is_pci_subsystem(dev)) {
				_pr_simple("vendor", dev->vendor);
				_pr_simple("device", dev->device);
			}
		}
		printf("\n");
	}
}

static inline void _print_key_value(const char* k, const char *v)
{
	printf("%-32s: %s\n", k, v);
}

static void print_ht(GHashTable *ht)
{
	GList *keys = g_hash_table_get_keys(ht);

	keys = g_list_sort(keys, (GCompareFunc) strcmp);
	while (keys) {
		char *k = (char *) keys->data;
		char *v = g_hash_table_lookup(ht, k);

		_print_key_value(k, v);
		keys = g_list_next(keys);
	}
	g_list_free(keys);
}

static void igt_devs_print_detail(struct igt_list_head *view)
{
	struct igt_device *dev;

	if (!view)
		return;

	if (igt_list_empty(view)) {
		printf("No GPU devices found\n");
		return;
	}

	igt_list_for_each_entry(dev, view, link) {
		printf("========== %s:%s ==========\n",
		       dev->subsystem, dev->syspath);
		if (!is_drm_subsystem(dev)) {
			_print_key_value("card device", dev->drm_card);
			_print_key_value("render device", dev->drm_render);
		}

		printf("\n[properties]\n");
		print_ht(dev->props_ht);
		printf("\n[attributes]\n");
		print_ht(dev->attrs_ht);
		printf("\n");
	}
}

static struct print_func {
	void (*prn)(struct igt_list_head *view);
} print_functions[] = {
	[IGT_PRINT_SIMPLE] = { .prn = igt_devs_print_simple },
	[IGT_PRINT_DETAIL] = { .prn = igt_devs_print_detail },
};

/**
 * igt_devices_print
 * @printtype: IGT_PRINT_SIMPLE or IGT_PRINT_DETAIL
 *
 * Function can be used by external tool to print device array in simple
 * or detailed form. This function is added here to avoid exposing
 * internal implementation data structures.
 */
void igt_devices_print(enum igt_devices_print_type printtype)
{
	print_functions[printtype].prn(&igt_devs.filtered);
}

/**
 * igt_devices_print_vendors
 *
 * Print pci id -> vendor mappings. Vendor names printed by this function
 * can be used for filters like pci which allows passing vendor - like
 * vendor id (8086) or as a string (Intel).
 */
void igt_devices_print_vendors(void)
{
	printf("Recognized vendors:\n");
	printf("%-8s %-16s\n", "PCI ID", "vendor");
	for (typeof(*pci_vendor_mapping) *vm = pci_vendor_mapping; vm->name; vm++) {
		printf("%-8s %-16s\n", vm->vendor_id, vm->name);
	}
}

struct filter;

/* ------------------------------------------------------------------------- */
struct filter_class {
	struct igt_list_head *(*filter_function)(const struct filter_class *fcls,
						 const struct filter *filter);
	bool (*is_valid)(const struct filter_class *fcls,
			 const struct filter *filter);
	const char *name;
	const char *help;
	const char *detail;
};

#define FILTER_NAME_LEN 32
#define FILTER_DATA_LEN 256

struct filter {
	struct filter_class *class;

	char raw_data[FILTER_DATA_LEN];

	struct {
		char *vendor;
		char *device;
		char *card;
		char *drm;
		char *driver;
	} data;
};

static void fill_filter_data(struct filter *filter, const char *key, const char *value)
{
	if (key == NULL || value == NULL) {
		return;
	}

#define __fill_key(name) if (strcmp(key, #name) == 0) \
	filter->data.name = strdup(value)
	__fill_key(vendor);
	__fill_key(device);
	__fill_key(card);
	__fill_key(drm);
	__fill_key(driver);
#undef __fill_key

}

static void split_filter_data(struct filter *filter)
{
	char *property, *key, *data, *dup;

	dup = strdup(filter->raw_data);
	data = dup;

	while ((property = strsep(&data, ","))) {
		key = strsep(&property, "=");
		fill_filter_data(filter, key, property);
	}

	free(dup);
}

static struct filter_class *get_filter_class(const char *class_name, const struct filter *filter);

static bool parse_filter(const char *fstr, struct filter *filter)
{
	char class_name[32];
	if (!fstr || !filter)
		return false;

	memset(filter, 0, sizeof(*filter));

	if (sscanf(fstr, "%31[^:]:%255s", class_name, filter->raw_data) >= 1) {
		filter->class = get_filter_class(class_name, filter);
		split_filter_data(filter);
		return true;
	}

	return false;
}

static bool is_vendor_matched(struct igt_device *dev, const char *vendor)
{
	const char *vendor_id;

	if (!dev->vendor || !vendor)
		return false;

	/* First we compare vendor id, like 8086 */
	if (!strcasecmp(dev->vendor, vendor))
		return true;

	/* Likely we have vendor string instead of id */
	vendor_id = get_pci_vendor_id_by_name(vendor);
	if (!vendor_id)
		return false;

	return !strcasecmp(dev->vendor, vendor_id);
}

/* Filter which matches subsystem:/sys/... path.
 * Used as first filter in chain.
 */
static struct igt_list_head *filter_sys(const struct filter_class *fcls,
					const struct filter *filter)
{
	struct igt_device *dev;
	(void) fcls;

	DBG("filter sys\n");
	if (!strlen(filter->raw_data))
		return &igt_devs.filtered;

	dev = igt_device_from_syspath(filter->raw_data);
	if (dev) {
		struct igt_device *dup = duplicate_device(dev);
		igt_list_add_tail(&dup->link, &igt_devs.filtered);
	}

	return &igt_devs.filtered;
}

/* Find drm device using direct path to /dev/dri/.
 * It extends filter_sys to allow using drm:/dev/dri/cardX and
 * drm:/dev/dri/renderDX filter syntax.
 */
static struct igt_list_head *filter_drm(const struct filter_class *fcls,
					const struct filter *filter)
{
	struct igt_device *dev;
	(void) fcls;

	DBG("filter drm\n");
	if (!strlen(filter->raw_data))
		return &igt_devs.filtered;

	igt_list_for_each_entry(dev, &igt_devs.all, link) {
		if (!is_drm_subsystem(dev))
			continue;

		if (strequal(dev->syspath, filter->raw_data) ||
			strequal(dev->drm_card, filter->raw_data) ||
			strequal(dev->drm_render, filter->raw_data)) {
			struct igt_device *dup = duplicate_device(dev);
			igt_list_add_tail(&dup->link, &igt_devs.filtered);
			break;
		}
	}


	return &igt_devs.filtered;
}

/* Find appropriate pci device matching vendor/device/card filter arguments.
 */
static struct igt_list_head *filter_pci(const struct filter_class *fcls,
					const struct filter *filter)
{
	struct igt_device *dev;
	int card = -1;
	(void) fcls;

	DBG("filter pci\n");

	if (filter->data.card) {
		sscanf(filter->data.card, "%d", &card);
		if (card < 0) {
			return &igt_devs.filtered;
		}
	} else {
		card = 0;
	}

	igt_list_for_each_entry(dev, &igt_devs.all, link) {
		if (!is_pci_subsystem(dev))
			continue;

		/* Skip if 'vendor' doesn't match (hex or name) */
		if (filter->data.vendor && !is_vendor_matched(dev, filter->data.vendor))
			continue;

		/* Skip if 'device' doesn't match */
		if (filter->data.device && strcasecmp(filter->data.device, dev->device))
			continue;

		/* We get n-th card */
		if (!card) {
			struct igt_device *dup = duplicate_device(dev);
			igt_list_add_tail(&dup->link, &igt_devs.filtered);
			break;
		}
		card--;
	}

	DBG("Filter pci filtered size: %d\n", igt_list_length(&igt_devs.filtered));

	return &igt_devs.filtered;
}

static bool sys_path_valid(const struct filter_class *fcls,
			   const struct filter *filter)
{
	struct stat st;

	if (stat(filter->raw_data, &st)) {
		igt_warn("sys_path_valid: syspath [%s], err: %s\n",
			 filter->raw_data, strerror(errno));
		return false;
	}

	return true;
}


static struct filter_class filter_definition_list[] = {
	{
		.name = "sys",
		.is_valid = sys_path_valid,
		.filter_function = filter_sys,
		.help = "sys:/sys/devices/pci0000:00/0000:00:02.0",
		.detail = "find device by its sysfs path\n",
	},
	{
		.name = "drm",
		.filter_function = filter_drm,
		.help = "drm:/dev/dri/* path",
		.detail = "find drm device by /dev/dri/* node\n",
	},
	{
		.name = "pci",
		.filter_function = filter_pci,
		.help = "pci:[vendor=%04x/name][,device=%04x][,card=%d]",
		.detail = "vendor is hex number or vendor name\n",
	},
	{
		.name = NULL,
	},
};

static struct filter_class *get_filter_class(const char *class_name, const struct filter *filter)
{
	struct filter_class *fcls = NULL;
	int i = 0;

	while ((fcls = &filter_definition_list[i++])->name != NULL) {
		if (strcmp(class_name, fcls->name) == 0)
			return fcls;
	}

	return NULL;
}

/**
 * @igt_device_print_filter_types
 *
 * Print all filters syntax for device selection.
 */
void igt_device_print_filter_types(void)
{
	const struct filter_class *filter = NULL;
	int i = 0;

	printf("Filter types:\n---\n");
	printf("%-12s  %s\n---\n", "filter", "syntax");

	while ((filter = &filter_definition_list[i++])->name != NULL) {
		printf("%-12s  %s\n", filter->name, filter->help);
		printf("%-12s  %s\n", "", filter->detail);
	}
}

struct device_filter {
	char filter[NAME_MAX];
	struct igt_list_head link;
};

static IGT_LIST_HEAD(device_filters);

/**
 * igt_device_filter_count
 *
 * Returns number of filters collected in the filter list.
 */
int igt_device_filter_count(void)
{
	return igt_list_length(&device_filters);
}

/* Check does filter is valid. It checks:
 * 1. /sys/... path first
 * 2. filter name from filter definition
 */
static bool is_filter_valid(const char *fstr)
{
	struct filter filter;
	int ret;

	ret = parse_filter(fstr, &filter);
	if (!ret)
		return false;

	if (filter.class == NULL) {
		igt_warn("No filter class matching [%s]\n", fstr);
		return false;
	}

	if (filter.class->is_valid != NULL && !filter.class->is_valid(filter.class, &filter))
	{
		igt_warn("Filter not valid [%s:%s]\n", filter.class->name, filter.raw_data);
		return false;
	}

	return true;
}

/**
 * igt_device_filter_add
 * @filters: filter(s) to be stored in filter array
 *
 * Function allows passing single or more filters within one string. This is
 * for CI when it can extract filter from environment variable (and it must
 * be single string). So if @filter contains semicolon ';' it treats
 * each part as separate filter and adds to the filter array.
 *
 * Returns number of filters added to filter array. Can be greater than
 * 1 if @filters contains more than one filter separated by semicolon.
 */
int igt_device_filter_add(const char *filters)
{
	char *dup, *dup_orig, *filter;
	int count = 0;

	dup = strdup(filters);
	dup_orig = dup;

	while ((filter = strsep(&dup, ";"))) {
		bool is_valid = is_filter_valid(filter);
		igt_warn_on(!is_valid);
		if (is_valid) {
			struct device_filter *df = malloc(sizeof(*df));
			strncpy(df->filter, filter, sizeof(df->filter)-1);
			igt_list_add_tail(&df->link, &device_filters);
			count++;
		}
	}

	free(dup_orig);

	return count;
}

/**
 * igt_device_filter_free_all
 *
 * Free all filters within array.
 */
void igt_device_filter_free_all(void)
{
	struct device_filter *filter, *tmp;

	igt_list_for_each_entry_safe(filter, tmp, &device_filters, link) {
		igt_list_del(&filter->link);
		free(filter);
	}
}

/**
 * igt_device_filter_get
 * @num: Number of filter from filter array
 *
 * Returns filter string or NULL if @num is out of range of filter array.
 */
const char *igt_device_filter_get(int num)
{
	struct device_filter *filter;
	int i = 0;

	if (num < 0)
		return NULL;


	igt_list_for_each_entry(filter, &device_filters, link) {
		if (i == num)
			return filter->filter;
		i++;
	}


	return NULL;
}

static bool igt_device_filter_apply(const char *fstr)
{
	struct igt_device *dev, *tmp;
	struct filter filter;
	bool ret;

	if (!fstr)
		return false;

	ret = parse_filter(fstr, &filter);
	if (!ret) {
		igt_warn("Can't split filter [%s]\n", fstr);
		return false;
	}

	/* Clean the filtered list */
	igt_list_for_each_entry_safe(dev, tmp, &igt_devs.filtered, link) {
		igt_list_del(&dev->link);
		free(dev);
	}

	/* If filter.data contains "/sys" use direct path instead
	 * contextual filter.
	 */

	if (!filter.class) {
		igt_warn("No filter class matching [%s]\n", fstr);
		return false;
	}
	filter.class->filter_function(filter.class, &filter);

	return true;
}

/**
 * igt_device_card_match
 * @filter: filter string
 * @card: pointer to igt_device_card struct
 *
 * Function applies filter to match device from device array.
 *
 * Returns:
 * false - no card pointer was passed or card wasn't matched,
 * true - card matched and returned.
 */
bool igt_device_card_match(const char *filter, struct igt_device_card *card)
{
	struct igt_device *dev = NULL;

	if (!card)
		return false;
	memset(card, 0, sizeof(*card));

	igt_devices_scan(false);

	if (igt_device_filter_apply(filter) == false)
		return false;

	if (igt_list_empty(&igt_devs.filtered))
		return false;

	/* We take first one if more than one card matches filter */
	dev = igt_list_first_entry(&igt_devs.filtered, dev, link);

	__copy_dev_to_card(dev, card);

	return true;
}

/**
 * igt_open_card:
 * @card: pointer to igt_device_card structure
 *
 * Open /dev/dri/cardX device represented by igt_device_card structure.
 * Requires filled @card argument (see igt_device_card_match() function).
 *
 * An open DRM fd or -1 on error
 */
int igt_open_card(struct igt_device_card *card)
{
	if (!card || !strlen(card->card))
		return -1;

	return open(card->card, O_RDWR);
}

/**
 * igt_open_render:
 * @card: pointer to igt_device_card structure
 *
 * Open /dev/dri/renderDX device represented by igt_device_card structure.
 * Requires filled @card argument (see igt_device_card_match() function).
 *
 * An open DRM fd or -1 on error
 */
int igt_open_render(struct igt_device_card *card)
{
	if (!card || !strlen(card->render))
		return -1;

	return open(card->render, O_RDWR);
}
