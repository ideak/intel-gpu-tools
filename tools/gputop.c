// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <termios.h>
#include <sys/sysmacros.h>
#include <stdbool.h>

#include "igt_drm_clients.h"
#include "igt_drm_fdinfo.h"

static const char *bars[] = { " ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█" };

static void n_spaces(const unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		putchar(' ');
}

static void print_percentage_bar(double percent, int max_len)
{
	int bar_len, i, len = max_len - 2;
	const int w = 8;

	assert(max_len > 0);

	bar_len = ceil(w * percent * len / 100.0);
	if (bar_len > w * len)
		bar_len = w * len;

	putchar('|');

	for (i = bar_len; i >= w; i -= w)
		printf("%s", bars[w]);
	if (i)
		printf("%s", bars[i]);

	len -= (bar_len + (w - 1)) / w;
	n_spaces(len);

	putchar('|');
}

static int
print_client_header(struct igt_drm_client *c, int lines, int con_w, int con_h,
		    int *engine_w)
{
	int ret, len;

	if (lines++ >= con_h)
		return lines;

	printf("\033[7m");
	ret = printf("DRM minor %u", c->drm_minor);
	n_spaces(con_w - ret);

	if (lines++ >= con_h)
		return lines;

	putchar('\n');
	len = printf("%*s %*s ",
		     c->clients->max_pid_len, "PID",
		     c->clients->max_name_len, "NAME");

	if (c->engines->num_engines) {
		unsigned int i;
		int width;

		*engine_w = width = (con_w - len) / c->engines->num_engines;

		for (i = 0; i <= c->engines->max_engine_id; i++) {
			const char *name = c->engines->names[i];
			int name_len = strlen(name);
			int pad = (width - name_len) / 2;
			int spaces = width - pad - name_len;

			if (!name)
				continue;

			if (pad < 0 || spaces < 0)
				continue;

			n_spaces(pad);
			printf("%s", name);
			n_spaces(spaces);
			len += pad + name_len + spaces;
		}
	}

	n_spaces(con_w - len);
	printf("\033[0m\n");

	return lines;
}


static bool
newheader(const struct igt_drm_client *c, const struct igt_drm_client *pc)
{
	return !pc || c->drm_minor != pc->drm_minor;
}

static int
print_client(struct igt_drm_client *c, struct igt_drm_client **prevc,
	     double t, int lines, int con_w, int con_h,
	     unsigned int period_us, int *engine_w)
{
	unsigned int i;

	/* Filter out idle clients. */
	if (!c->total_runtime || c->samples < 2)
		return lines;

	/* Print header when moving to a different DRM card. */
	if (newheader(c, *prevc)) {
		lines = print_client_header(c, lines, con_w, con_h, engine_w);
		if (lines >= con_h)
			return lines;
	}

	*prevc = c;

	printf("%*s %*s ",
	       c->clients->max_pid_len, c->pid_str,
	       c->clients->max_name_len, c->print_name);
	lines++;

	for (i = 0; c->samples > 1 && i <= c->engines->max_engine_id; i++) {
		double pct;

		if (!c->engines->capacity[i])
			continue;

		pct = (double)c->val[i] / period_us / 1e3 * 100 /
		      c->engines->capacity[i];

		/*
		 * Guard against fluctuations between our scanning period and
		 * GPU times as exported by the kernel in fdinfo.
		 */
		if (pct > 100.0)
			pct = 100.0;

		print_percentage_bar(pct, *engine_w);
	}

	putchar('\n');

	return lines;
}

static int
__client_id_cmp(const struct igt_drm_client *a,
		const struct igt_drm_client *b)
{
	if (a->id > b->id)
		return 1;
	else if (a->id < b->id)
		return -1;
	else
		return 0;
}

static int client_cmp(const void *_a, const void *_b, void *unused)
{
	const struct igt_drm_client *a = _a;
	const struct igt_drm_client *b = _b;
	long val_a, val_b;

	/* DRM cards into consecutive buckets first. */
	val_a = a->drm_minor;
	val_b = b->drm_minor;
	if (val_a > val_b)
		return 1;
	else if (val_b > val_a)
		return -1;

	/*
	 * Within buckets sort by last sampling period aggregated runtime, with
	 * client id as a tie-breaker.
	 */
	val_a = a->last_runtime;
	val_b = b->last_runtime;
	if (val_a == val_b)
		return __client_id_cmp(a, b);
	else if (val_b > val_a)
		return 1;
	else
		return -1;

}

int main(int argc, char **argv)
{
	unsigned int period_us = 2e6;
	struct igt_drm_clients *clients = NULL;
	int con_w = -1, con_h = -1;

	clients = igt_drm_clients_init(NULL);
	if (!clients)
		exit(1);

	igt_drm_clients_scan(clients, NULL, NULL, 0);

	for (;;) {
		struct igt_drm_client *c, *prevc = NULL;
		int i, engine_w = 0, lines = 0;
		struct winsize ws;

		if (ioctl(0, TIOCGWINSZ, &ws) != -1) {
			con_w = ws.ws_col;
			con_h = ws.ws_row;
			if (con_w == 0 && con_h == 0) {
				/* Serial console. */
				con_w = 80;
				con_h = 24;
			}
		}

		igt_drm_clients_scan(clients, NULL, NULL, 0);
		igt_drm_clients_sort(clients, client_cmp);

		printf("\033[H\033[J");

		igt_for_each_drm_client(clients, c, i) {
			assert(c->status != IGT_DRM_CLIENT_PROBE);
			if (c->status != IGT_DRM_CLIENT_ALIVE)
				break; /* Active clients are first in the array. */

			lines = print_client(c, &prevc, (double)period_us / 1e6,
					     lines, con_w, con_h, period_us,
					     &engine_w);
			if (lines >= con_h)
				break;
		}

		if (lines++ < con_h)
			printf("\n");

		usleep(period_us);
	}

	return 0;
}
