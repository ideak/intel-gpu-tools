/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef IGT_RUNNERCOMMS_H
#define IGT_RUNNERCOMMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A flat struct that can and will be directly dumped to
 * disk. Constructed with runnerpacket_<type>() helper functions.
 */
struct runnerpacket {
	uint32_t size; /* Full size of the packet in octets */
	uint32_t type; /* runnerpacket_type, but fixed width */
	int32_t senderpid;
	int32_t sendertid;

	char data[];
} __attribute__((packed));

_Static_assert(sizeof(struct runnerpacket) == 4 * 4, "runnerpacket structure must not change");
_Static_assert(offsetof(struct runnerpacket, data) == 4 * 4, "runnerpacket structure must not change");

/*
 * A helper for reading and parsing runnerpacket structs. Fields will
 * point directly into the data field of an existing runnerpacket
 * object. Constructed with read_runnerpacket().
 *
 * Some fields can be left as 0 / NULL / some other applicable invalid
 * value in the case of having older dumps read with binaries that
 * have extended the data formats.
 */
typedef union runnerpacket_read_helper {
	/*
	 * All other fields must begin with "uint32_t type" so it's a
	 * common initial sequence, safe to read no matter what union
	 * field is active.
	 */
	uint32_t type;

	struct {
		uint32_t type;

		uint8_t stream;
		const char *text;
	} log;

	struct {
		uint32_t type;

		const char *cmdline;
	} exec;

	struct {
		uint32_t type;

		int32_t exitcode;
		const char *timeused;
	} exit;

	struct {
		uint32_t type;

		const char *name;
	} subteststart;

	struct {
		uint32_t type;

		const char *name;
		const char *result;
		const char *timeused;
		const char *reason;
	} subtestresult;

	struct {
		uint32_t type;

		const char *name;
	} dynamicsubteststart;

	struct {
		uint32_t type;

		const char *name;
		const char *result;
		const char *timeused;
		const char *reason;
	} dynamicsubtestresult;

	struct {
		uint32_t type;

		const char *text;
	} versionstring;

	struct {
		uint32_t type;

		const char *result;
	} resultoverride;
} runnerpacket_read_helper;

void set_runner_socket(int fd);
bool runner_connected(void);
void send_to_runner(struct runnerpacket *packet);

runnerpacket_read_helper read_runnerpacket(const struct runnerpacket *packet);

/*
 * All packet types must document the format of the data[] array. The
 * notation used is
 *
 * Explanation of the packet
 * type: explanation of values
 * type2: explanation of values
 * (etc)
 *
 * The type "cstring" can be used to denote that the content is a
 * nul-terminated string.
 */
enum runnerpacket_type {
      PACKETTYPE_INVALID,
      /* No data. This type is only used on parse failures and such. */

      PACKETTYPE_LOG,
      /*
       * Normal log message.
       * uint8_t: 1 = stdout, 2 = stderr
       * cstring: Log text
       */

      PACKETTYPE_EXEC,
      /*
       * Command line executed. Sent by runner before calling exec().
       * cstring: command line as one string, argv[0] included, space separated
       */

      PACKETTYPE_EXIT,
      /*
       * Process exit. Written by runner.
       * int32_t: exitcode
       * cstring: Time taken by the process from exec to exit, as a floating point value in seconds, as text
       */

      PACKETTYPE_SUBTEST_START,
      /*
       * Subtest begins.
       * cstring: Name of the subtest
       */

      PACKETTYPE_SUBTEST_RESULT,
      /*
       * Subtest ends. Can appear without a corresponding SUBTEST_START packet.
       * cstring: Name of the subtest
       * cstring: Result of the subtest
       * cstring: Time taken by the subtest, as a floating point value in seconds, as text
       * cstring: If len > 0, the reason for the subtest result (fail/skip)
       */

      PACKETTYPE_DYNAMIC_SUBTEST_START,
      /*
       * Dynamic subtest begins.
       * cstring: Name of the dynamic subtest
       */

      PACKETTYPE_DYNAMIC_SUBTEST_RESULT,
      /*
       * Dynamic subtest ends.
       * cstring: Name of the dynamic subtest
       * cstring: Result of the dynamic subtest
       * cstring: Time taken by the dynamic subtest, as a floating point value in seconds, as text
       * cstring: If len > 0, the reason for the dynamic subtest result (fail/skip)
       */

      PACKETTYPE_VERSIONSTRING,
      /*
       * Version of the running test
       * cstring: Version string
       */

      PACKETTYPE_RESULT_OVERRIDE,
      /*
       * Override the result of the most recently started test/subtest/dynamic subtest. Used for timeout and abort etc.
       * cstring: The result to use, as text. All lowercase.
       */


      PACKETTYPE_NUM_TYPES /* must be last */
};

struct runnerpacket *runnerpacket_log(uint8_t stream, const char *text);
struct runnerpacket *runnerpacket_exec(char **argv);
struct runnerpacket *runnerpacket_exit(int32_t exitcode, const char *timeused);
struct runnerpacket *runnerpacket_subtest_start(const char *name);
struct runnerpacket *runnerpacket_subtest_result(const char *name, const char *result,
						 const char *timeused, const char *reason);
struct runnerpacket *runnerpacket_dynamic_subtest_start(const char *name);
struct runnerpacket *runnerpacket_dynamic_subtest_result(const char *name, const char *result,
							 const char *timeused, const char *reason);
struct runnerpacket *runnerpacket_versionstring(const char *text);
struct runnerpacket *runnerpacket_resultoverride(const char *result);

uint32_t socket_dump_canary(void);

struct runnerpacket_log_sig_safe {
	uint32_t size;
	uint32_t type;
	int32_t senderpid;
	int32_t sendertid;

	uint8_t stream;
	char data[128];
} __attribute__((packed));

_Static_assert(offsetof(struct runnerpacket_log_sig_safe, stream) == 4 * 4, "signal-safe log runnerpacket must be compatible");
_Static_assert(offsetof(struct runnerpacket_log_sig_safe, data) == 4 * 4 + 1, "signal-safe log runnerpacket must be compatible");

void log_to_runner_sig_safe(const char *str, size_t len);

/*
 * Comms dump reader
 *
 * A visitor for reading comms dump files. Calls handlers if
 * corresponding handler is set. Reading stops if a handler returns
 * false.
 *
 * The passed arguments are the packet itself, the already-constructed
 * read helper, and the userdata pointer from the visitor.
 */
typedef bool (*handler_t)(const struct runnerpacket *, runnerpacket_read_helper, void *userdata);

struct comms_visitor {
	handler_t log;
	handler_t exec;
	handler_t exit;
	handler_t subtest_start;
	handler_t subtest_result;
	handler_t dynamic_subtest_start;
	handler_t dynamic_subtest_result;
	handler_t versionstring;
	handler_t result_override;

	void* userdata;
};

enum {
	COMMSPARSE_ERROR,
	COMMSPARSE_EMPTY,
	COMMSPARSE_SUCCESS
};
int comms_read_dump(int fd, struct comms_visitor *visitor);

#endif
