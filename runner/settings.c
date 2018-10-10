#include "settings.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
	OPT_ABORT_ON_ERROR,
	OPT_TEST_LIST,
	OPT_IGNORE_MISSING,
	OPT_PIGLIT_DMESG,
	OPT_OVERALL_TIMEOUT,
	OPT_HELP = 'h',
	OPT_NAME = 'n',
	OPT_DRY_RUN = 'd',
	OPT_INCLUDE = 't',
	OPT_EXCLUDE = 'x',
	OPT_SYNC = 's',
	OPT_LOG_LEVEL = 'l',
	OPT_OVERWRITE = 'o',
	OPT_MULTIPLE = 'm',
	OPT_TIMEOUT = 'c',
	OPT_WATCHDOG = 'g',
};

static struct {
	int level;
	const char *name;
} log_levels[] = {
	{ LOG_LEVEL_NORMAL, "normal" },
	{ LOG_LEVEL_QUIET, "quiet" },
	{ LOG_LEVEL_VERBOSE, "verbose" },
	{ 0, 0 },
};

static bool set_log_level(struct settings* settings, const char *level)
{
	typeof(*log_levels) *it;

	for (it = log_levels; it->name; it++) {
		if (!strcmp(level, it->name)) {
			settings->log_level = it->level;
			return true;
		}
	}

	return false;
}

static const char *usage_str =
	"usage: runner [options] [test_root] results-path\n\n"
	"Options:\n"
	" Piglit compatible:\n"
	"  -h, --help            Show this help message and exit\n"
	"  -n <test name>, --name <test name>\n"
	"                        Name of this test run\n"
	"  -d, --dry-run         Do not execute the tests\n"
	"  -t <regex>, --include-tests <regex>\n"
	"                        Run only matching tests (can be used more than once)\n"
	"  -x <regex>, --exclude-tests <regex>\n"
	"                        Exclude matching tests (can be used more than once)\n"
	"  --abort-on-monitored-error\n"
	"                        Abort execution when a fatal condition is detected.\n"
	"                        <TODO>\n"
	"  -s, --sync            Sync results to disk after every test\n"
	"  -l {quiet,verbose,dummy}, --log-level {quiet,verbose,dummy}\n"
	"                        Set the logger verbosity level\n"
	"  --test-list TEST_LIST\n"
	"                        A file containing a list of tests to run\n"
	"  -o, --overwrite       If the results-path already exists, delete it\n"
	"  --ignore-missing      Ignored but accepted, for piglit compatibility\n"
	"\n"
	" Incompatible options:\n"
	"  -m, --multiple-mode   Run multiple subtests in the same binary execution.\n"
	"                        If a testlist file is given, consecutive subtests are\n"
	"                        run in the same execution if they are from the same\n"
	"                        binary. Note that in that case relative ordering of the\n"
	"                        subtest execution is dictated by the test binary, not\n"
	"                        the testlist\n"
	"  --inactivity-timeout <seconds>\n"
	"                        Kill the running test after <seconds> of inactivity in\n"
	"                        the test's stdout, stderr, or dmesg\n"
	"  --overall-timeout <seconds>\n"
	"                        Don't execute more tests after <seconds> has elapsed\n"
	"  --use-watchdog        Use hardware watchdog for lethal enforcement of the\n"
	"                        above timeout. Killing the test process is still\n"
	"                        attempted at timeout trigger.\n"
	"  --piglit-style-dmesg  Filter dmesg like piglit does. Piglit considers matches\n"
	"                        against a short filter list to mean the test result\n"
	"                        should be changed to dmesg-warn/dmesg-fail. Without\n"
	"                        this option everything except matches against a\n"
	"                        (longer) filter list means the test result should\n"
	"                        change.\n"
	"  [test_root]           Directory that contains the IGT tests. The environment\n"
	"                        variable IGT_TEST_ROOT will be used if set, overriding\n"
	"                        this option if given.\n"
	;

static void usage(const char *extra_message, FILE *f)
{
	if (extra_message)
		fprintf(f, "%s\n\n", extra_message);

	fputs(usage_str, f);
}

static bool add_regex(struct regex_list *list, char *new)
{
	regex_t *regex;
	size_t buflen;
	char *buf;
	int s;

	regex = malloc(sizeof(*regex));

	if ((s = regcomp(regex, new,
			 REG_EXTENDED | REG_NOSUB)) != 0) {
		buflen = regerror(s, regex, NULL, 0);
		buf = malloc(buflen);
		regerror(s, regex, buf, buflen);
		usage(buf, stderr);

		free(buf);
		regfree(regex);
		free(regex);
		return false;
	}

	list->regexes = realloc(list->regexes,
				(list->size + 1) * sizeof(*list->regexes));
	list->regex_strings = realloc(list->regex_strings,
				      (list->size + 1) * sizeof(*list->regex_strings));
	list->regexes[list->size] = regex;
	list->regex_strings[list->size] = new;
	list->size++;

	return true;
}

static void free_regexes(struct regex_list *regexes)
{
	size_t i;

	for (i = 0; i < regexes->size; i++) {
		free(regexes->regex_strings[i]);
		regfree(regexes->regexes[i]);
		free(regexes->regexes[i]);
	}
	free(regexes->regex_strings);
	free(regexes->regexes);
}

static bool readable_file(char *filename)
{
	return !access(filename, R_OK);
}

void init_settings(struct settings *settings)
{
	memset(settings, 0, sizeof(*settings));
}

void free_settings(struct settings *settings)
{
	free(settings->test_list);
	free(settings->name);
	free(settings->test_root);
	free(settings->results_path);

	free_regexes(&settings->include_regexes);
	free_regexes(&settings->exclude_regexes);

	init_settings(settings);
}

bool parse_options(int argc, char **argv,
		   struct settings *settings)
{
	int c;
	char *env_test_root;

	static struct option long_options[] = {
		{"help", no_argument, NULL, OPT_HELP},
		{"name", required_argument, NULL, OPT_NAME},
		{"dry-run", no_argument, NULL, OPT_DRY_RUN},
		{"include-tests", required_argument, NULL, OPT_INCLUDE},
		{"exclude-tests", required_argument, NULL, OPT_EXCLUDE},
		{"abort-on-monitored-error", no_argument, NULL, OPT_ABORT_ON_ERROR},
		{"sync", no_argument, NULL, OPT_SYNC},
		{"log-level", required_argument, NULL, OPT_LOG_LEVEL},
		{"test-list", required_argument, NULL, OPT_TEST_LIST},
		{"overwrite", no_argument, NULL, OPT_OVERWRITE},
		{"ignore-missing", no_argument, NULL, OPT_IGNORE_MISSING},
		{"multiple-mode", no_argument, NULL, OPT_MULTIPLE},
		{"inactivity-timeout", required_argument, NULL, OPT_TIMEOUT},
		{"overall-timeout", required_argument, NULL, OPT_OVERALL_TIMEOUT},
		{"use-watchdog", no_argument, NULL, OPT_WATCHDOG},
		{"piglit-style-dmesg", no_argument, NULL, OPT_PIGLIT_DMESG},
		{ 0, 0, 0, 0},
	};

	free_settings(settings);

	optind = 1;

	while ((c = getopt_long(argc, argv, "hn:dt:x:sl:om", long_options, NULL)) != -1) {
		switch (c) {
		case OPT_HELP:
			usage(NULL, stdout);
			goto error;
		case OPT_NAME:
			settings->name = strdup(optarg);
			break;
		case OPT_DRY_RUN:
			settings->dry_run = true;
			break;
		case OPT_INCLUDE:
			if (!add_regex(&settings->include_regexes, strdup(optarg)))
				goto error;
			break;
		case OPT_EXCLUDE:
			if (!add_regex(&settings->exclude_regexes, strdup(optarg)))
				goto error;
			break;
		case OPT_ABORT_ON_ERROR:
			settings->abort_on_error = true;
			break;
		case OPT_SYNC:
			settings->sync = true;
			break;
		case OPT_LOG_LEVEL:
			if (!set_log_level(settings, optarg)) {
				usage("Cannot parse log level", stderr);
				goto error;
			}
			break;
		case OPT_TEST_LIST:
			settings->test_list = absolute_path(optarg);
			break;
		case OPT_OVERWRITE:
			settings->overwrite = true;
			break;
		case OPT_IGNORE_MISSING:
			/* Ignored, piglit compatibility */
			break;
		case OPT_MULTIPLE:
			settings->multiple_mode = true;
			break;
		case OPT_TIMEOUT:
			settings->inactivity_timeout = atoi(optarg);
			break;
		case OPT_OVERALL_TIMEOUT:
			settings->overall_timeout = atoi(optarg);
			break;
		case OPT_WATCHDOG:
			settings->use_watchdog = true;
			break;
		case OPT_PIGLIT_DMESG:
			settings->piglit_style_dmesg = true;
			break;
		case '?':
			usage(NULL, stderr);
			goto error;
		default:
			usage("Cannot parse options", stderr);
			goto error;
		}
	}

	switch (argc - optind) {
	case 2:
		settings->test_root = absolute_path(argv[optind]);
		++optind;
		/* fallthrough */
	case 1:
		settings->results_path = absolute_path(argv[optind]);
		break;
	case 0:
		usage("Results-path missing", stderr);
		goto error;
	default:
		usage("Extra arguments after results-path", stderr);
		goto error;
	}

	if ((env_test_root = getenv("IGT_TEST_ROOT")) != NULL) {
		free(settings->test_root);
		settings->test_root = absolute_path(env_test_root);
	}

	if (!settings->test_root) {
		usage("Test root not set", stderr);
		goto error;
	}

	if (!settings->name) {
		char *name = strdup(settings->results_path);
		settings->name = strdup(basename(name));
		free(name);
	}

	return true;

 error:
	free_settings(settings);
	return false;
}

bool validate_settings(struct settings *settings)
{
	int dirfd, fd;

	if (settings->test_list && !readable_file(settings->test_list)) {
		usage("Cannot open test-list file", stderr);
		return false;
	}

	if (!settings->results_path) {
		usage("No results-path set; this shouldn't happen", stderr);
		return false;
	}

	if (!settings->test_root) {
		usage("No test root set; this shouldn't happen", stderr);
		return false;
	}

	dirfd = open(settings->test_root, O_DIRECTORY | O_RDONLY);
	if (dirfd < 0) {
		fprintf(stderr, "Test directory %s cannot be opened\n", settings->test_root);
		return false;
	}

	fd = openat(dirfd, "test-list.txt", O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Cannot open %s/test-list.txt\n", settings->test_root);
		close(dirfd);
		return false;
	}

	close(fd);
	close(dirfd);

	return true;
}

static char *_dirname(const char *path)
{
	char *tmppath = strdup(path);
	char *tmpname = dirname(tmppath);
	tmpname = strdup(tmpname);
	free(tmppath);
	return tmpname;
}

static char *_basename(const char *path)
{
	char *tmppath = strdup(path);
	char *tmpname = basename(tmppath);
	tmpname = strdup(tmpname);
	free(tmppath);
	return tmpname;
}

char *absolute_path(char *path)
{
	char *result = NULL;
	char *base, *dir;
	char *ret;

	result = realpath(path, NULL);
	if (result != NULL)
		return result;

	dir = _dirname(path);
	ret = absolute_path(dir);
	free(dir);

	base = _basename(path);
	asprintf(&result, "%s/%s", ret, base);
	free(base);
	free(ret);

	return result;
}

static char settings_filename[] = "metadata.txt";
bool serialize_settings(struct settings *settings)
{
#define SERIALIZE_LINE(f, s, name, format) fprintf(f, "%s : " format "\n", #name, s->name)

	int dirfd, fd;
	FILE *f;

	if (!settings->results_path) {
		usage("No results-path set; this shouldn't happen", stderr);
		return false;
	}

	if ((dirfd = open(settings->results_path, O_DIRECTORY | O_RDONLY)) < 0) {
		mkdir(settings->results_path, 0755);
		if ((dirfd = open(settings->results_path, O_DIRECTORY | O_RDONLY)) < 0) {
			usage("Creating results-path failed", stderr);
			return false;
		}
	}

	if (!settings->overwrite &&
	    faccessat(dirfd, settings_filename, F_OK, 0) == 0) {
		usage("Settings metadata already exists and not overwriting", stderr);
		return false;
	}

	if (settings->overwrite &&
	    unlinkat(dirfd, settings_filename, 0) != 0 &&
	    errno != ENOENT) {
		usage("Error removing old settings metadata", stderr);
		return false;
	}

	if ((fd = openat(dirfd, settings_filename, O_CREAT | O_EXCL | O_WRONLY, 0666)) < 0) {
		char *msg;

		asprintf(&msg, "Creating settings serialization file failed: %s", strerror(errno));
		usage(msg, stderr);

		free(msg);
		close(dirfd);
		return false;
	}

	f = fdopen(fd, "w");
	if (!f) {
		close(fd);
		close(dirfd);
		return false;
	}

	SERIALIZE_LINE(f, settings, abort_on_error, "%d");
	if (settings->test_list)
		SERIALIZE_LINE(f, settings, test_list, "%s");
	if (settings->name)
		SERIALIZE_LINE(f, settings, name, "%s");
	SERIALIZE_LINE(f, settings, dry_run, "%d");
	SERIALIZE_LINE(f, settings, sync, "%d");
	SERIALIZE_LINE(f, settings, log_level, "%d");
	SERIALIZE_LINE(f, settings, overwrite, "%d");
	SERIALIZE_LINE(f, settings, multiple_mode, "%d");
	SERIALIZE_LINE(f, settings, inactivity_timeout, "%d");
	SERIALIZE_LINE(f, settings, overall_timeout, "%d");
	SERIALIZE_LINE(f, settings, use_watchdog, "%d");
	SERIALIZE_LINE(f, settings, piglit_style_dmesg, "%d");
	SERIALIZE_LINE(f, settings, test_root, "%s");
	SERIALIZE_LINE(f, settings, results_path, "%s");

	if (settings->sync) {
		fsync(fd);
		fsync(dirfd);
	}

	fclose(f);
	close(dirfd);
	return true;

#undef SERIALIZE_LINE
}

bool read_settings(struct settings *settings, int dirfd)
{
#define PARSE_LINE(s, name, val, field, write) \
	if (!strcmp(name, #field)) {	       \
		s->field = write;	       \
		free(name);		       \
		free(val);		       \
		name = val = NULL;	       \
		continue;		       \
	}

	int fd;
	FILE *f;
	char *name = NULL, *val = NULL;

	free_settings(settings);

	if ((fd = openat(dirfd, settings_filename, O_RDONLY)) < 0)
		return false;

	f = fdopen(fd, "r");
	if (!f) {
		close(fd);
		return false;
	}

	while (fscanf(f, "%ms : %ms", &name, &val) == 2) {
		int numval = atoi(val);
		PARSE_LINE(settings, name, val, abort_on_error, numval);
		PARSE_LINE(settings, name, val, test_list, val ? strdup(val) : NULL);
		PARSE_LINE(settings, name, val, name, val ? strdup(val) : NULL);
		PARSE_LINE(settings, name, val, dry_run, numval);
		PARSE_LINE(settings, name, val, sync, numval);
		PARSE_LINE(settings, name, val, log_level, numval);
		PARSE_LINE(settings, name, val, overwrite, numval);
		PARSE_LINE(settings, name, val, multiple_mode, numval);
		PARSE_LINE(settings, name, val, inactivity_timeout, numval);
		PARSE_LINE(settings, name, val, overall_timeout, numval);
		PARSE_LINE(settings, name, val, use_watchdog, numval);
		PARSE_LINE(settings, name, val, piglit_style_dmesg, numval);
		PARSE_LINE(settings, name, val, test_root, val ? strdup(val) : NULL);
		PARSE_LINE(settings, name, val, results_path, val ? strdup(val) : NULL);

		printf("Warning: Unknown field in settings file: %s = %s\n",
		       name, val);
		free(name);
		free(val);
		name = val = NULL;
	}

	free(name);
	free(val);
	fclose(f);

	return true;

#undef PARSE_LINE
}
