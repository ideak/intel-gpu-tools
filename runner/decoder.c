#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "runnercomms.h"

static bool handle_log(const struct runnerpacket *packet, runnerpacket_read_helper helper, void *userdata)
{
	printf("(pid=%d tid=%d) LOG\tstream=%d,text=%s",
	       packet->senderpid, packet->sendertid,
	       helper.log.stream, helper.log.text);
	if (strlen(helper.log.text) == 0 || helper.log.text[strlen(helper.log.text) - 1] != '\n')
		printf("\n");

	return true;
}

static bool handle_exec(const struct runnerpacket *packet, runnerpacket_read_helper helper, void *userdata)
{
	printf("(pid=%d tid=%d) EXEC\tcmdline=%s\n",
	       packet->senderpid, packet->sendertid,
	       helper.exec.cmdline);

	return true;
}

static bool handle_exit(const struct runnerpacket *packet, runnerpacket_read_helper helper, void *userdata)
{
	printf("(pid=%d tid=%d) EXIT\texitcode=%d,timeused=%s\n",
	       packet->senderpid, packet->sendertid,
	       helper.exit.exitcode, helper.exit.timeused);

	return true;
}

static bool handle_subtest_start(const struct runnerpacket *packet, runnerpacket_read_helper helper, void *userdata)
{
	printf("(pid=%d tid=%d) SUBTEST_START\tname=%s\n",
	       packet->senderpid, packet->sendertid,
	       helper.subteststart.name);

	return true;
}

static bool handle_subtest_result(const struct runnerpacket *packet, runnerpacket_read_helper helper, void *userdata)
{
	printf("(pid=%d tid=%d) SUBTEST_RESULT\tname=%s,result=%s,timeused=%s,reason=%s\n",
	       packet->senderpid, packet->sendertid,
	       helper.subtestresult.name,
	       helper.subtestresult.result,
	       helper.subtestresult.timeused,
	       helper.subtestresult.reason ?: "<null>");

	return true;
}

static bool handle_dynamic_subtest_start(const struct runnerpacket *packet, runnerpacket_read_helper helper, void *userdata)
{
	printf("(pid=%d tid=%d) DYNAMIC_SUBTEST_START\tname=%s\n",
	       packet->senderpid, packet->sendertid,
	       helper.dynamicsubteststart.name);

	return true;
}

static bool handle_dynamic_subtest_result(const struct runnerpacket *packet, runnerpacket_read_helper helper, void *userdata)
{
	printf("(pid=%d tid=%d) DYNAMIC_SUBTEST_RESULT\tname=%s,result=%s,timeused=%s,reason=%s\n",
	       packet->senderpid, packet->sendertid,
	       helper.dynamicsubtestresult.name,
	       helper.dynamicsubtestresult.result,
	       helper.dynamicsubtestresult.timeused,
	       helper.dynamicsubtestresult.reason ?: "<null>");

	return true;
}

static bool handle_versionstring(const struct runnerpacket *packet, runnerpacket_read_helper helper, void *userdata)
{
	printf("(pid=%d tid=%d) VERSIONSTRING\ttext=%s",
	       packet->senderpid, packet->sendertid,
	       helper.versionstring.text);
	if (strlen(helper.versionstring.text) == 0 || helper.versionstring.text[strlen(helper.versionstring.text) - 1] != '\n')
		printf("\n");

	return true;
}

static bool handle_result_override(const struct runnerpacket *packet, runnerpacket_read_helper helper, void *userdata)
{
	printf("pid=%d tid=%d) RESULT_OVERRIDE\tresult=%s\n",
	       packet->senderpid, packet->sendertid,
	       helper.resultoverride.result);

	return true;
}

struct comms_visitor logger = {
	.log = handle_log,
	.exec = handle_exec,
	.exit = handle_exit,
	.subtest_start = handle_subtest_start,
	.subtest_result = handle_subtest_result,
	.dynamic_subtest_start = handle_dynamic_subtest_start,
	.dynamic_subtest_result = handle_dynamic_subtest_result,
	.versionstring = handle_versionstring,
	.result_override = handle_result_override,
};

int main(int argc, char **argv)
{
	int fd;

	if (argc < 2) {
		printf("Usage: %s igt-comms-data-file\n", argv[0]);
		return 2;
	}

	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Failure opening %s: %m\n", argv[1]);
		return 1;
	}

	comms_read_dump(fd, &logger);

	return 0;
}
