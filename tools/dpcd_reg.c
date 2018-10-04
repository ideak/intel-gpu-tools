/*
 * Copyright © 2018 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * DPCD register read/write tool
 * This tool wraps around DRM_DP_AUX_DEV module to provide DPCD register read
 * and write, so CONFIG_DRM_DP_AUX_DEV needs to be set.
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <unistd.h>
#include <limits.h>
#include <stdbool.h>

#define MAX_DP_OFFSET	0xfffff
#define DRM_AUX_MINORS	256

const char aux_dev[] = "/dev/drm_dp_aux";

struct dpcd_data {
	int devid;
	int file_op;
	uint32_t offset;
	enum command {
		INVALID = -1,
		READ = 2,
		WRITE,
	} cmd;
	size_t count;
	uint8_t val;
};

static void print_usage(void)
{
	printf("Usage: dpcd_reg [OPTION ...] COMMAND\n\n");
	printf("COMMAND is one of:\n");
	printf("  read:		Read [count] bytes dpcd reg at an offset\n");
	printf("  write:	Write a dpcd reg at an offset\n\n");
	printf("Options for the above COMMANDS are\n");
	printf(" --device=DEVID		Aux device id, as listed in /dev/drm_dp_aux_dev[n]. Defaults to 0. Upper limit - 256\n");
	printf(" --offset=REG_ADDR	DPCD register offset in hex. Defaults to 0x0. Upper limit - 0xfffff\n");
	printf(" --count=BYTES		For reads, specify number of bytes to be read from the offset. Defaults to 1\n");
	printf(" --value		For writes, specify a hex value to be written. Upper limit - 0xff\n\n");

	printf(" --help: print the usage\n");
}

static inline bool strtol_err_util(char *endptr, long *val)
{
	if (*endptr != '\0' || *val < 0 ||
	    (*val == LONG_MAX && errno == ERANGE))
		return true;

	return false;
}

static int parse_opts(struct dpcd_data *dpcd, int argc, char **argv)
{
	int ret, vflag = 0;
	long temp;
	char *endptr;

	struct option longopts[] = {
		{ "count",	required_argument,	NULL,		'c' },
		{ "device",	required_argument,	NULL,		'd' },
		{ "help",	no_argument,		NULL,		'h' },
		{ "offset",	required_argument,	NULL,		'o' },
		{ "value",	required_argument,	NULL,		'v' },
		{ 0 }
	};

	while ((ret = getopt_long(argc, argv, "-:c:d:ho:v:", longopts, NULL)) != -1) {
		switch (ret) {
		case 'c':
			temp = strtol(optarg, &endptr, 10);
			if (strtol_err_util(endptr, &temp)) {
				fprintf(stderr,
					"--count argument is invalid/negative/out-of-range\n");
				print_usage();
				return EXIT_FAILURE;
			}
			dpcd->count = temp;
			break;
		case 'd':
			temp = strtol(optarg, &endptr, 10);
			if (strtol_err_util(endptr, &temp) ||
					    temp > DRM_AUX_MINORS) {
				fprintf(stderr,
					"--device argument is invalid/negative/out-of-range\n");
				print_usage();
				return ERANGE;
			}
			dpcd->devid = temp;
			break;
		case 'h':
			printf("DPCD register read and write tool\n\n");
			printf("This tool requires CONFIG_DRM_DP_AUX_CHARDEV\n"
				"to be set in the kernel config.\n\n");
			print_usage();
			exit(EXIT_SUCCESS);
		case 'o':
			temp = strtol(optarg, &endptr, 16);
			if (strtol_err_util(endptr, &temp) ||
					    temp > MAX_DP_OFFSET) {
				fprintf(stderr,
					"--offset argument is invalid/negative/out-of-range\n");
				print_usage();
				return ERANGE;
			}
			dpcd->offset = temp;
			break;
		case 'v':
			vflag = 'v';
			temp = strtol(optarg, &endptr, 16);
			if (strtol_err_util(endptr, &temp) || temp > 0xff) {
				fprintf(stderr,
					"--value argument is invalid/negative/out-of-range\n");
				print_usage();
				return ERANGE;
			}
			dpcd->val = temp;
			break;
		/* Command parsing */
		case 1:
			if (strcmp(optarg, "read") == 0) {
				temp = READ;
			} else if (strcmp(optarg, "write") == 0) {
				temp = WRITE;
				dpcd->file_op = O_WRONLY;
			} else {
				fprintf(stderr, "Unrecognized command\n");
				print_usage();
				return EXIT_FAILURE;
			}
			dpcd->cmd = temp;
			break;
		case ':':
			fprintf(stderr, "Option -%c requires an argument\n",
				 optopt);
			print_usage();
			return EXIT_FAILURE;
		default:
			fprintf(stderr, "Invalid option\n");
			print_usage();
			return EXIT_FAILURE;
		}
	}

	if ((dpcd->count + dpcd->offset) > (MAX_DP_OFFSET + 1)) {
		fprintf(stderr, "Out of bounds. Count + Offset <= 0x100000\n");
		return ERANGE;
	}

	if ((dpcd->cmd == WRITE) && (vflag != 'v')) {
		fprintf(stderr, "Write value is missing\n");
		print_usage();
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

static int dpcd_read(int fd, uint32_t offset, size_t count)
{
	int ret = EXIT_SUCCESS, pret, i;
	uint8_t *buf = calloc(count, sizeof(uint8_t));

	if (!buf) {
		fprintf(stderr, "Can't allocate read buffer\n");
		return ENOMEM;
	}

	pret = pread(fd, buf, count, offset);
	if (pret < 0) {
		fprintf(stderr, "Failed to read - %s\n", strerror(errno));
		ret = errno;
		goto out;
	}

	if (pret < count) {
		fprintf(stderr,
			"Read %u byte(s), expected %zu bytes, starting at offset %x\n\n", pret, count, offset);
		ret = EXIT_FAILURE;
	}

	printf("0x%02x: ", offset);
	for (i = 0; i < pret; i++)
		printf(" %02x", *(buf + i));
	printf("\n");

out:
	free(buf);
	return ret;
}

static int dpcd_write(int fd, uint32_t offset, uint8_t val)
{
	int ret = EXIT_SUCCESS, pret;

	pret = pwrite(fd, (const void *)&val, sizeof(uint8_t), offset);
	if (pret < 0) {
		fprintf(stderr, "Failed to write - %s\n", strerror(errno));
		ret = errno;
	} else if (pret == 0) {
		fprintf(stderr, "Zero bytes were written\n");
		ret = EXIT_FAILURE;
	}

	return ret;
}

int main(int argc, char **argv)
{
	char dev_name[20];
	int ret, fd;

	struct dpcd_data dpcd = {
		.devid = 0,
		.file_op = O_RDONLY,
		.offset = 0x0,
		.cmd = INVALID,
		.count = 1,
	};

	ret = parse_opts(&dpcd, argc, argv);
	if (ret != EXIT_SUCCESS)
		return ret;

	snprintf(dev_name, strlen(aux_dev) + 4, "%s%d", aux_dev, dpcd.devid);

	fd = open(dev_name, dpcd.file_op);
	if (fd < 0) {
		fprintf(stderr,
			"Failed to open %s aux device - error: %s\n", dev_name,
			strerror(errno));
		return errno;
	}

	switch (dpcd.cmd) {
	case READ:
		ret = dpcd_read(fd, dpcd.offset, dpcd.count);
		break;
	case WRITE:
		ret = dpcd_write(fd, dpcd.offset, dpcd.val);
		break;
	default:
		fprintf(stderr, "Please specify a command: read/write.\n");
		print_usage();
		ret = EXIT_FAILURE;
		break;
	}

	close(fd);

	return ret;
}
