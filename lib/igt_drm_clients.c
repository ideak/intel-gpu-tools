// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include "igt_drm_clients.h"
#include "igt_drm_fdinfo.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(array) (sizeof(array) / sizeof(array[0]))
#endif

/**
 * igt_drm_clients_init:
 * @private_data: private data to store in the struct
 *
 * Allocate and initialise the clients structure to be used with further API
 * calls.
 */
struct igt_drm_clients *igt_drm_clients_init(void *private_data)
{
	struct igt_drm_clients *clients;

	clients = malloc(sizeof(*clients));
	if (!clients)
		return NULL;

	memset(clients, 0, sizeof(*clients));

	clients->private_data = private_data;

	return clients;
}

static struct igt_drm_client *
igt_drm_clients_find(struct igt_drm_clients *clients,
		     enum igt_drm_client_status status,
		     unsigned int drm_minor, unsigned int id)
{
	unsigned int start, num;
	struct igt_drm_client *c;

	start = status == IGT_DRM_CLIENT_FREE ? clients->active_clients : 0; /* Free block at the end. */
	num = clients->num_clients - start;

	for (c = &clients->client[start]; num; c++, num--) {
		if (status != c->status)
			continue;

		if (status == IGT_DRM_CLIENT_FREE ||
		    (drm_minor == c->drm_minor && c->id == id))
			return c;
	}

	return NULL;
}

static void
igt_drm_client_update(struct igt_drm_client *c, unsigned int pid, char *name,
		      const struct drm_client_fdinfo *info)
{
	unsigned int i;
	int len;

	/* Update client pid if it changed (fd sharing). */
	if (c->pid != pid) {
		c->pid = pid;
		len = snprintf(c->pid_str, sizeof(c->pid_str) - 1, "%u", pid);
		if (len > c->clients->max_pid_len)
			c->clients->max_pid_len = len;
	}

	/* Update client name if it changed (fd sharing). */
	if (strcmp(c->name, name)) {
		char *p;

		strncpy(c->name, name, sizeof(c->name) - 1);
		strncpy(c->print_name, name, sizeof(c->print_name) - 1);

		p = c->print_name;
		while (*p) {
			if (!isprint(*p))
				*p = '*';
			p++;
		}

		len = strlen(c->print_name);
		if (len > c->clients->max_name_len)
			c->clients->max_name_len = len;
	}

	c->last_runtime = 0;
	c->total_runtime = 0;

	for (i = 0; i < c->clients->num_classes; i++) {
		assert(i < ARRAY_SIZE(info->busy));

		if (info->busy[i] < c->last[i])
			continue; /* It will catch up soon. */

		c->total_runtime += info->busy[i];
		c->val[i] = info->busy[i] - c->last[i];
		c->last_runtime += c->val[i];
		c->last[i] = info->busy[i];
	}

	c->samples++;
	c->status = IGT_DRM_CLIENT_ALIVE;
}

static void
igt_drm_client_add(struct igt_drm_clients *clients,
		   const struct drm_client_fdinfo *info,
		   unsigned int pid, char *name, unsigned int drm_minor)
{
	struct igt_drm_client *c;

	assert(!igt_drm_clients_find(clients, IGT_DRM_CLIENT_ALIVE,
				     drm_minor, info->id));

	c = igt_drm_clients_find(clients, IGT_DRM_CLIENT_FREE, 0, 0);
	if (!c) {
		unsigned int idx = clients->num_clients;

		/*
		 * Grow the array a bit past the current requirement to avoid
		 * constant reallocation when clients are dynamically appearing
		 * and disappearing.
		 */
		clients->num_clients += (clients->num_clients + 2) / 2;
		clients->client = realloc(clients->client,
					  clients->num_clients * sizeof(*c));
		assert(clients->client);

		c = &clients->client[idx];
		memset(c, 0, (clients->num_clients - idx) * sizeof(*c));
	}

	c->id = info->id;
	c->drm_minor = drm_minor;
	c->clients = clients;
	c->val = calloc(clients->num_classes, sizeof(c->val));
	c->last = calloc(clients->num_classes, sizeof(c->last));
	assert(c->val && c->last);

	igt_drm_client_update(c, pid, name, info);
}

static
void igt_drm_client_free(struct igt_drm_client *c, bool clear)
{
	free(c->val);
	free(c->last);

	if (clear)
		memset(c, 0, sizeof(*c));
}

/**
 * igt_drm_clients_sort:
 * @clients: Previously initialised clients object
 * @cmp: Client comparison callback
 *
 * Sort the clients array according to the passed in comparison callback which
 * is compatible with the qsort(3) semantics.
 *
 * Caller has to ensure the callback is putting all active
 * (IGT_DRM_CLIENT_ALIVE) clients in a single group at the head of the array
 * before any other sorting criteria.
 */
struct igt_drm_clients *
igt_drm_clients_sort(struct igt_drm_clients *clients,
		     int (*cmp)(const void *, const void *))
{
	unsigned int active, free;
	struct igt_drm_client *c;
	int tmp;

	if (!clients)
		return clients;

	qsort(clients->client, clients->num_clients, sizeof(*clients->client),
	      cmp);

	/* Trim excessive array space. */
	active = 0;
	igt_for_each_drm_client(clients, c, tmp) {
		if (c->status != IGT_DRM_CLIENT_ALIVE)
			break; /* Active clients are first in the array. */
		active++;
	}

	clients->active_clients = active;

	/* Trim excess free space when clients are exiting. */
	free = clients->num_clients - active;
	if (free > clients->num_clients / 2) {
		active = clients->num_clients - free / 2;
		if (active != clients->num_clients) {
			clients->num_clients = active;
			clients->client = realloc(clients->client,
						  clients->num_clients *
						  sizeof(*c));
		}
	}

	return clients;
}

/**
 * igt_drm_clients_free:
 * @clients: Previously initialised clients object
 *
 * Free all clients and all memory associated with the clients structure.
 */
void igt_drm_clients_free(struct igt_drm_clients *clients)
{
	struct igt_drm_client *c;
	unsigned int tmp;

	igt_for_each_drm_client(clients, c, tmp)
		igt_drm_client_free(c, false);

	free(clients->client);
	free(clients);
}

static DIR *opendirat(int at, const char *name)
{
	DIR *dir;
	int fd;

	fd = openat(at, name, O_DIRECTORY);
	if (fd < 0)
		return NULL;

	dir = fdopendir(fd);
	if (!dir)
		close(fd);

	return dir;
}

static size_t readat2buf(int at, const char *name, char *buf, const size_t sz)
{
	ssize_t count;
	int fd;

	fd = openat(at, name, O_RDONLY);
	if (fd <= 0)
		return 0;

	count = read(fd, buf, sz - 1);
	close(fd);

	if (count > 0) {
		buf[count] = 0;

		return count;
	} else {
		buf[0] = 0;

		return 0;
	}
}

static bool get_task_name(const char *buffer, char *out, unsigned long sz)
{
	char *s = index(buffer, '(');
	char *e = rindex(buffer, ')');
	unsigned int len;

	if (!s || !e)
		return false;
	assert(e >= s);

	len = e - ++s;
	if(!len || (len + 1) >= sz)
		return false;

	strncpy(out, s, len);
	out[len] = 0;

	return true;
}

static bool is_drm_fd(int fd_dir, const char *name, unsigned int *minor)
{
	struct stat stat;
	int ret;

	ret = fstatat(fd_dir, name, &stat, 0);

	if (ret == 0 &&
	    (stat.st_mode & S_IFMT) == S_IFCHR &&
	    major(stat.st_rdev) == 226) {
		*minor = minor(stat.st_rdev);
		return true;
	}

	return false;
}

static void clients_update_max_lengths(struct igt_drm_clients *clients)
{
	struct igt_drm_client *c;
	int tmp;

	clients->max_name_len = 0;
	clients->max_pid_len = 0;

	igt_for_each_drm_client(clients, c, tmp) {
		int len;

		if (c->status != IGT_DRM_CLIENT_ALIVE)
			continue; /* Array not yet sorted by the caller. */

		len = strlen(c->print_name);
		if (len > clients->max_name_len)
			clients->max_name_len = len;

		len = strlen(c->pid_str);
		if (len > clients->max_pid_len)
			clients->max_pid_len = len;
	}
}

/**
 * igt_drm_clients_scan:
 * @clients: Previously initialised clients object
 * @filter_client: Callback for client filtering
 * @name_map: Array of engine name strings
 * @map_entries: Number of items in the @name_map array
 *
 * Scan all open file descriptors from all processes in order to find all DRM
 * clients and manage our internal list.
 *
 * If @name_map is provided each found engine in the fdinfo struct must
 * correspond to one of the provided names. In this case the index of the engine
 * stats tracked in struct igt_drm_client will be tracked under the same index
 * as the engine name provided.
 *
 * If @name_map is not provided engine names will be auto-detected (this is
 * less performant) and indices will correspond with auto-detected names as
 * listed int clients->engine_class[].
 */
struct igt_drm_clients *
igt_drm_clients_scan(struct igt_drm_clients *clients,
		     bool (*filter_client)(const struct igt_drm_clients *,
					   const struct drm_client_fdinfo *),
		     const char **name_map, unsigned int map_entries)
{
	struct dirent *proc_dent;
	struct igt_drm_client *c;
	bool freed = false;
	DIR *proc_dir;
	int tmp;

	if (!clients)
		return clients;

	/*
	 * First mark all alive clients as 'probe' so we can figure out which
	 * ones have existed since the previous scan.
	 */
	igt_for_each_drm_client(clients, c, tmp) {
		assert(c->status != IGT_DRM_CLIENT_PROBE);
		if (c->status == IGT_DRM_CLIENT_ALIVE)
			c->status = IGT_DRM_CLIENT_PROBE;
		else
			break; /* Free block at the end of array. */
	}

	proc_dir = opendir("/proc");
	if (!proc_dir)
		return clients;

	while ((proc_dent = readdir(proc_dir)) != NULL) {
		unsigned int client_pid, minor = 0;
		int pid_dir = -1, fd_dir = -1;
		struct dirent *fdinfo_dent;
		char client_name[64] = { };
		DIR *fdinfo_dir = NULL;
		char buf[4096];
		size_t count;

		if (proc_dent->d_type != DT_DIR)
			continue;
		if (!isdigit(proc_dent->d_name[0]))
			continue;

		pid_dir = openat(dirfd(proc_dir), proc_dent->d_name,
				 O_DIRECTORY | O_RDONLY);
		if (pid_dir < 0)
			continue;

		count = readat2buf(pid_dir, "stat", buf, sizeof(buf));
		if (!count)
			goto next;

		client_pid = atoi(buf);
		if (!client_pid)
			goto next;

		if (!get_task_name(buf, client_name, sizeof(client_name)))
			goto next;

		fd_dir = openat(pid_dir, "fd", O_DIRECTORY | O_RDONLY);
		if (fd_dir < 0)
			goto next;

		fdinfo_dir = opendirat(pid_dir, "fdinfo");
		if (!fdinfo_dir)
			goto next;

		while ((fdinfo_dent = readdir(fdinfo_dir)) != NULL) {
			struct drm_client_fdinfo info = { };

			if (fdinfo_dent->d_type != DT_REG)
				continue;
			if (!isdigit(fdinfo_dent->d_name[0]))
				continue;

			if (!is_drm_fd(fd_dir, fdinfo_dent->d_name, &minor))
				continue;

			if (!__igt_parse_drm_fdinfo(dirfd(fdinfo_dir),
						    fdinfo_dent->d_name, &info,
						    name_map, map_entries))
				continue;

			if (filter_client && !filter_client(clients, &info))
				continue;

			if (igt_drm_clients_find(clients, IGT_DRM_CLIENT_ALIVE,
						 minor, info.id))
				continue; /* Skip duplicate fds. */

			c = igt_drm_clients_find(clients, IGT_DRM_CLIENT_PROBE,
						 minor, info.id);
			if (!c)
				igt_drm_client_add(clients, &info, client_pid,
						   client_name, minor);
			else
				igt_drm_client_update(c, client_pid,
						      client_name, &info);
		}

next:
		if (fdinfo_dir)
			closedir(fdinfo_dir);
		if (fd_dir >= 0)
			close(fd_dir);
		if (pid_dir >= 0)
			close(pid_dir);
	}

	closedir(proc_dir);

	/*
	 * Clients still in 'probe' status after the scan have exited and need
	 * to be freed.
	 */
	igt_for_each_drm_client(clients, c, tmp) {
		if (c->status == IGT_DRM_CLIENT_PROBE) {
			igt_drm_client_free(c, true);
			freed = true;
		} else if (c->status == IGT_DRM_CLIENT_FREE) {
			break;
		}
	}

	if (freed)
		clients_update_max_lengths(clients);

	return clients;
}
