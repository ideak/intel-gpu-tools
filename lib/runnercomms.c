/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "igt_aux.h"
#include "runnercomms.h"

/**
 * SECTION:runnercomms
 * @short_description: Structured communication to igt_runner
 * @title: runnercomms
 * @include: runnercomms.h
 *
 * This library provides means for the tests to communicate to
 * igt_runner with a formally specified protocol, avoiding
 * shortcomings and pain points of text-based communication.
 */

static sig_atomic_t runner_socket_fd = -1;

/**
 * set_runner_socket:
 * @fd: socket connected to runner
 *
 * If the passed fd is a valid socket, globally sets it to be the fd
 * to use to talk to igt_runner.
 */
void set_runner_socket(int fd)
{
	struct stat sb;

	if (fstat(fd, &sb))
		return;

	if (!S_ISSOCK(sb.st_mode))
		return;

	/*
	 * We only sanity-check that the fd is a socket. We don't
	 * check that it's a datagram socket etc.
	 */

	runner_socket_fd = fd;
}

/**
 * runner_connected:
 *
 * Returns whether set_runner_socket has been called with a valid
 * socket fd. Note: Will be true forever after that point. This
 * function is used to mainly determine whether log strings will be
 * output to the socket or to stdout/stderr and that cannot be changed
 * even if the socket is lost midway.
 */
bool runner_connected(void)
{
	return runner_socket_fd >= 0;
}

/**
 * send_to_runner:
 * @packet: packet to send
 *
 * Sends the given communications packet to igt_runner. Calls free()
 * on the packet, don't reuse it.
 */
void send_to_runner(struct runnerpacket *packet)
{
	if (runner_connected())
		write(runner_socket_fd, packet, packet->size);
	free(packet);
}

/* If enough data left, copy the data to dst, advance p, reduce size */
static void read_integer(void* dst, size_t bytes, const char **p, uint32_t *size)
{
	if (*size < bytes) {
		*size = 0;
		return;
	}

	memcpy(dst, *p, bytes);
	*p += bytes;
	*size -= bytes;
}

/* If nul-termination can be found, set dststr to point to the cstring, advance p, reduce size */
static void read_cstring(const char **dststr, const char **p, uint32_t *size)
{
	const char *end;

	end = memchr(*p, '\0', *size);
	if (end == NULL) {
		*size = 0;
		return;
	}

	*dststr = *p;
	*size -= end - *p + 1;
	*p = end + 1;
}

/**
 * read_runnerpacket:
 * @packet: runner communications packet to read
 *
 * Checks that the internal data of the communications packet is valid
 * and the contents can safely be inspected without further checking
 * for out-of-bounds etc. Constructs a runnerpacket_read_helper which
 * will, for c-style strings, point to various sub-values directly in
 * the #data field within @packet. Those are valid only as long as
 * @packet is valid.
 *
 * Returns: An appropriately constructed runnerpacket_read_helper. On
 * data validation errors, the #type of the returned value will be
 * #PACKETTYPE_INVALID.
 */
runnerpacket_read_helper read_runnerpacket(const struct runnerpacket *packet)
{
	runnerpacket_read_helper ret = {};
	uint32_t sizeleft;
	const char *p;

	if (packet->size < sizeof(*packet)) {
		ret.type = PACKETTYPE_INVALID;
		return ret;
	}

	ret.type = packet->type;
	sizeleft = packet->size - sizeof(*packet);
	p = packet->data;

	switch (packet->type) {
	case PACKETTYPE_LOG:
		read_integer(&ret.log.stream, sizeof(ret.log.stream), &p, &sizeleft);
		read_cstring(&ret.log.text, &p, &sizeleft);

		if (ret.log.text == NULL)
			ret.type = PACKETTYPE_INVALID;

		break;
	case PACKETTYPE_EXEC:
		read_cstring(&ret.exec.cmdline, &p, &sizeleft);

		if (ret.exec.cmdline == NULL)
			ret.type = PACKETTYPE_INVALID;

		break;
	case PACKETTYPE_EXIT:
		read_integer(&ret.exit.exitcode, sizeof(ret.exit.exitcode), &p, &sizeleft);
		read_cstring(&ret.exit.timeused, &p, &sizeleft);

		break;
	case PACKETTYPE_SUBTEST_START:
		read_cstring(&ret.subteststart.name, &p, &sizeleft);

		if (ret.subteststart.name == NULL)
			ret.type = PACKETTYPE_INVALID;

		break;
	case PACKETTYPE_SUBTEST_RESULT:
		read_cstring(&ret.subtestresult.name, &p, &sizeleft);
		read_cstring(&ret.subtestresult.result, &p, &sizeleft);
		read_cstring(&ret.subtestresult.timeused, &p, &sizeleft);
		read_cstring(&ret.subtestresult.reason, &p, &sizeleft);

		if (ret.subtestresult.name == NULL ||
		    ret.subtestresult.result == NULL)
			ret.type = PACKETTYPE_INVALID;

		break;
	case PACKETTYPE_DYNAMIC_SUBTEST_START:
		read_cstring(&ret.dynamicsubteststart.name, &p, &sizeleft);

		if (ret.dynamicsubteststart.name == NULL)
			ret.type = PACKETTYPE_INVALID;

		break;
	case PACKETTYPE_DYNAMIC_SUBTEST_RESULT:
		read_cstring(&ret.dynamicsubtestresult.name, &p, &sizeleft);
		read_cstring(&ret.dynamicsubtestresult.result, &p, &sizeleft);
		read_cstring(&ret.dynamicsubtestresult.timeused, &p, &sizeleft);
		read_cstring(&ret.dynamicsubtestresult.reason, &p, &sizeleft);

		if (ret.dynamicsubtestresult.name == NULL ||
		    ret.dynamicsubtestresult.result == NULL)
			ret.type = PACKETTYPE_INVALID;

		break;
	case PACKETTYPE_VERSIONSTRING:
		read_cstring(&ret.versionstring.text, &p, &sizeleft);

		if (ret.versionstring.text == NULL)
			ret.type = PACKETTYPE_INVALID;

		break;
	case PACKETTYPE_RESULT_OVERRIDE:
		read_cstring(&ret.resultoverride.result, &p, &sizeleft);

		if (ret.resultoverride.result == NULL)
			ret.type = PACKETTYPE_INVALID;

		break;
	default:
		ret.type = PACKETTYPE_INVALID;
		break;
	}

	return ret;
}

struct runnerpacket *runnerpacket_log(uint8_t stream, const char *text)
{
	struct runnerpacket *packet;
	uint32_t size;
	char *p;

	size = sizeof(struct runnerpacket) + sizeof(stream) + strlen(text) + 1;
	packet = malloc(size);

	packet->size = size;
	packet->type = PACKETTYPE_LOG;
	packet->senderpid = getpid();
	packet->sendertid = gettid();

	p = packet->data;

	memcpy(p, &stream, sizeof(stream));
	p += sizeof(stream);

	strcpy(p, text);
	p += strlen(text) + 1;

	return packet;
}

struct runnerpacket *runnerpacket_exec(char **argv)
{
	struct runnerpacket *packet;
	uint32_t size;
	char *p;
	int i;

	size = sizeof(struct runnerpacket);

	for (i = 0; argv[i] != NULL; i++)
		size += strlen(argv[i]) + 1; // followed by a space of \0 so +1 either way for each

	packet = malloc(size);

	packet->size = size;
	packet->type = PACKETTYPE_EXEC;
	packet->senderpid = getpid();
	packet->sendertid = gettid();

	p = packet->data;

	for (i = 0; argv[i] != NULL; i++) {
		if (i != 0)
			*p++ = ' ';

		strcpy(p, argv[i]);
		p += strlen(argv[i]);
	}
	p[0] = '\0';

	return packet;
}

struct runnerpacket *runnerpacket_exit(int32_t exitcode, const char *timeused)
{
	struct runnerpacket *packet;
	uint32_t size;
	char *p;

	size = sizeof(struct runnerpacket) + sizeof(exitcode) + strlen(timeused) + 1;
	packet = malloc(size);

	packet->size = size;
	packet->type = PACKETTYPE_EXIT;
	packet->senderpid = getpid();
	packet->sendertid = gettid();

	p = packet->data;

	memcpy(p, &exitcode, sizeof(exitcode));
	p += sizeof(exitcode);

	strcpy(p, timeused);
	p += strlen(timeused) + 1;

	return packet;
}

struct runnerpacket *runnerpacket_subtest_start(const char *name)
{
	struct runnerpacket *packet;
	uint32_t size;
	char *p;

	size = sizeof(struct runnerpacket) + strlen(name) + 1;
	packet = malloc(size);

	packet->size = size;
	packet->type = PACKETTYPE_SUBTEST_START;
	packet->senderpid = getpid();
	packet->sendertid = gettid();

	p = packet->data;

	strcpy(p, name);
	p += strlen(name) + 1;

	return packet;
}

struct runnerpacket *runnerpacket_subtest_result(const char *name, const char *result,
						 const char *timeused, const char *reason)
{
	struct runnerpacket *packet;
	uint32_t size;
	char *p;

	if (reason == NULL)
		reason = "";

	size = sizeof(struct runnerpacket) + strlen(name) + strlen(result) + strlen(timeused) + strlen(reason) + 4;
	packet = malloc(size);

	packet->size = size;
	packet->type = PACKETTYPE_SUBTEST_RESULT;
	packet->senderpid = getpid();
	packet->sendertid = gettid();

	p = packet->data;

	strcpy(p, name);
	p += strlen(name) + 1;

	strcpy(p, result);
	p += strlen(result) + 1;

	strcpy(p, timeused);
	p += strlen(timeused) + 1;

	strcpy(p, reason);
	p += strlen(reason) + 1;

	return packet;
}

struct runnerpacket *runnerpacket_dynamic_subtest_start(const char *name)
{
	struct runnerpacket *packet;
	uint32_t size;
	char *p;

	size = sizeof(struct runnerpacket) + strlen(name) + 1;
	packet = malloc(size);

	packet->size = size;
	packet->type = PACKETTYPE_DYNAMIC_SUBTEST_START;
	packet->senderpid = getpid();
	packet->sendertid = gettid();

	p = packet->data;

	strcpy(p, name);
	p += strlen(name) + 1;

	return packet;
}

struct runnerpacket *runnerpacket_dynamic_subtest_result(const char *name, const char *result,
							 const char *timeused, const char *reason)
{
	struct runnerpacket *packet;
	uint32_t size;
	char *p;

	if (reason == NULL)
		reason = "";

	size = sizeof(struct runnerpacket) + strlen(name) + strlen(result) + strlen(timeused) + strlen(reason) + 4;
	packet = malloc(size);

	packet->size = size;
	packet->type = PACKETTYPE_DYNAMIC_SUBTEST_RESULT;
	packet->senderpid = getpid();
	packet->sendertid = gettid();

	p = packet->data;

	strcpy(p, name);
	p += strlen(name) + 1;

	strcpy(p, result);
	p += strlen(result) + 1;

	strcpy(p, timeused);
	p += strlen(timeused) + 1;

	strcpy(p, reason);
	p += strlen(reason) + 1;

	return packet;
}

struct runnerpacket *runnerpacket_versionstring(const char *text)
{
	struct runnerpacket *packet;
	uint32_t size;
	char *p;

	size = sizeof(struct runnerpacket) + strlen(text) + 1;
	packet = malloc(size);

	packet->size = size;
	packet->type = PACKETTYPE_VERSIONSTRING;
	packet->senderpid = getpid();
	packet->sendertid = gettid();

	p = packet->data;

	strcpy(p, text);
	p += strlen(text) + 1;

	return packet;
}

struct runnerpacket *runnerpacket_resultoverride(const char *result)
{
	struct runnerpacket *packet;
	uint32_t size;
	char *p;

	size = sizeof(struct runnerpacket) + strlen(result) + 1;
	packet = malloc(size);

	packet->size = size;
	packet->type = PACKETTYPE_RESULT_OVERRIDE;
	packet->senderpid = getpid();
	packet->sendertid = gettid();

	p = packet->data;

	strcpy(p, result);
	p += strlen(result) + 1;

	return packet;
}

uint32_t socket_dump_canary(void)
{
	return 'I' << 24 | 'G' << 16 | 'T' << 8 | '1';
}

void log_to_runner_sig_safe(const char *str, size_t len)
{
	size_t prlen = len;

	struct runnerpacket_log_sig_safe p = {
					      .size = sizeof(struct runnerpacket) + sizeof(uint8_t),
					      .type = PACKETTYPE_LOG,
					      .senderpid = getpid(),
					      .sendertid = 0, /* gettid() not signal safe */
					      .stream = STDERR_FILENO,
	};

	if (len > sizeof(p.data) - 1)
		prlen = sizeof(p.data) - 1;
	memcpy(p.data, str, prlen);
	p.size += prlen + 1;

	write(runner_socket_fd, &p, p.size);

	len -= prlen;
	if (len)
		log_to_runner_sig_safe(str + prlen, len);
}

/**
 * comms_read_dump:
 * @fd: Open fd to a comms dump file
 * @visitor: Collection of packet handlers
 *
 * Reads a comms dump file, calling specified handler functions for
 * individual packets.
 *
 * Returns: #COMMSPARSE_ERROR for failures reading or parsing the
 * dump, #COMMSPARSE_EMPTY for empty dumps (no comms used),
 * #COMMSPARSE_SUCCESS for successful read.
 */
int comms_read_dump(int fd, struct comms_visitor *visitor)
{
	struct stat statbuf;
	char *buf, *bufend, *p;
	int ret = COMMSPARSE_EMPTY;
	bool cont = true;

	if (fd < 0)
		return COMMSPARSE_EMPTY;

	if (fstat(fd, &statbuf))
		return COMMSPARSE_ERROR;

	if (statbuf.st_size == 0)
		return COMMSPARSE_EMPTY;

	buf = mmap(NULL, statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (buf == MAP_FAILED)
		return COMMSPARSE_ERROR;

	bufend = buf + statbuf.st_size;
	p = buf;

	while (p != NULL && p != bufend && cont) {
		const struct runnerpacket *packet;
		runnerpacket_read_helper helper;

		if (bufend - p >= sizeof(uint32_t)) {
			uint32_t canary;

			memcpy(&canary, p, sizeof(canary));
			if (canary != socket_dump_canary()) {
				fprintf(stderr,
					"Invalid canary while parsing comms: %"PRIu32", expected %"PRIu32"\n",
					canary, socket_dump_canary());
				munmap(buf, statbuf.st_size);
				return COMMSPARSE_ERROR;
			}
		}
		p += sizeof(uint32_t);

		if (bufend -p < sizeof(struct runnerpacket)) {
			fprintf(stderr,
				"Error parsing comms: Expected runnerpacket after canary, truncated file?\n");
			munmap(buf, statbuf.st_size);
			return COMMSPARSE_ERROR;
		}

		packet = (struct runnerpacket *)p;
		if (bufend -p < packet->size) {
			fprintf(stderr,
				"Error parsing comms: Unexpected end of file, truncated file?\n");
			munmap(buf, statbuf.st_size);
			return COMMSPARSE_ERROR;
		}
		p += packet->size;

		/*
		 * Runner sends EXEC itself before executing the test.
		 * If we get other types, it indicates the test really
		 * uses socket comms.
		 */
		if (packet->type != PACKETTYPE_EXEC)
			ret = COMMSPARSE_SUCCESS;

		switch (packet->type) {
		case PACKETTYPE_INVALID:
			printf("Warning: Unknown packet type %"PRIu32", skipping\n", packet->type);
			break;
		case PACKETTYPE_LOG:
			if (visitor->log) {
				helper = read_runnerpacket(packet);
				cont = visitor->log(packet, helper, visitor->userdata);
			}
			break;
		case PACKETTYPE_EXEC:
			if (visitor->exec) {
				helper = read_runnerpacket(packet);
				cont = visitor->exec(packet, helper, visitor->userdata);
			}
			break;
		case PACKETTYPE_EXIT:
			if (visitor->exit) {
				helper = read_runnerpacket(packet);
				cont = visitor->exit(packet, helper, visitor->userdata);
			}
			break;
		case PACKETTYPE_SUBTEST_START:
			if (visitor->subtest_start) {
				helper = read_runnerpacket(packet);
				cont = visitor->subtest_start(packet, helper, visitor->userdata);
			}
			break;
		case PACKETTYPE_SUBTEST_RESULT:
			if (visitor->subtest_result) {
				helper = read_runnerpacket(packet);
				cont = visitor->subtest_result(packet, helper, visitor->userdata);
			}
			break;
		case PACKETTYPE_DYNAMIC_SUBTEST_START:
			if (visitor->dynamic_subtest_start) {
				helper = read_runnerpacket(packet);
				cont = visitor->dynamic_subtest_start(packet, helper, visitor->userdata);
			}
			break;
		case PACKETTYPE_DYNAMIC_SUBTEST_RESULT:
			if (visitor->dynamic_subtest_result) {
				helper = read_runnerpacket(packet);
				cont = visitor->dynamic_subtest_result(packet, helper, visitor->userdata);
			}
			break;
		case PACKETTYPE_VERSIONSTRING:
			if (visitor->versionstring) {
				helper = read_runnerpacket(packet);
				cont = visitor->versionstring(packet, helper, visitor->userdata);
			}
			break;
		case PACKETTYPE_RESULT_OVERRIDE:
			if (visitor->result_override) {
				helper = read_runnerpacket(packet);
				cont = visitor->result_override(packet, helper, visitor->userdata);
			}
			break;
		default:
			printf("Warning: Unknown packet type %"PRIu32"\n", helper.type);
			break;
		}
	}

	munmap(buf, statbuf.st_size);
	return cont ? ret : COMMSPARSE_ERROR;
}
