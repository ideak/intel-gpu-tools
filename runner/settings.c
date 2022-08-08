#include "settings.h"
#include "version.h"

#include <ctype.h>
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
#include <sys/utsname.h>
#include <unistd.h>

enum {
	OPT_ABORT_ON_ERROR,
	OPT_DISK_USAGE_LIMIT,
	OPT_TEST_LIST,
	OPT_IGNORE_MISSING,
	OPT_PIGLIT_DMESG,
	OPT_DMESG_WARN_LEVEL,
	OPT_OVERALL_TIMEOUT,
	OPT_PER_TEST_TIMEOUT,
	OPT_ALLOW_NON_ROOT,
	OPT_CODE_COV_SCRIPT,
	OPT_ENABLE_CODE_COVERAGE,
	OPT_COV_RESULTS_PER_TEST,
	OPT_VERSION,
	OPT_PRUNE_MODE,
	OPT_HELP = 'h',
	OPT_NAME = 'n',
	OPT_DRY_RUN = 'd',
	OPT_INCLUDE = 't',
	OPT_EXCLUDE = 'x',
	OPT_ENVIRONMENT = 'e',
	OPT_SYNC = 's',
	OPT_LOG_LEVEL = 'l',
	OPT_OVERWRITE = 'o',
	OPT_MULTIPLE = 'm',
	OPT_TIMEOUT = 'c',
	OPT_WATCHDOG = 'g',
	OPT_BLACKLIST = 'b',
	OPT_LIST_ALL = 'L',
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

static struct {
	int value;
	const char *name;
} abort_conditions[] = {
	{ ABORT_TAINT, "taint" },
	{ ABORT_LOCKDEP, "lockdep" },
	{ ABORT_PING, "ping" },
	{ ABORT_ALL, "all" },
	{ 0, 0 },
};

static struct {
	int value;
	const char *name;
} prune_modes[] = {
	{ PRUNE_KEEP_DYNAMIC, "keep-dynamic-subtests" },
	{ PRUNE_KEEP_DYNAMIC, "keep-dynamic" },
	{ PRUNE_KEEP_SUBTESTS, "keep-subtests" },
	{ PRUNE_KEEP_ALL, "keep-all" },
	{ PRUNE_KEEP_REQUESTED, "keep-requested" },
	{ 0, 0 },
};

static const char settings_filename[] = "metadata.txt";
static const char env_filename[] = "environment.txt";

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

static bool set_abort_condition(struct settings* settings, const char *cond)
{
	typeof(*abort_conditions) *it;

	if (!cond) {
		settings->abort_mask = ABORT_ALL;
		return true;
	}

	if (strlen(cond) == 0) {
		settings->abort_mask = 0;
		return true;
	}

	for (it = abort_conditions; it->name; it++) {
		if (!strcmp(cond, it->name)) {
			settings->abort_mask |= it->value;
			return true;
		}
	}

	return false;
}

static bool set_prune_mode(struct settings* settings, const char *mode)
{
	typeof(*prune_modes) *it;

	for (it = prune_modes; it->name; it++) {
		if (!strcmp(mode, it->name)) {
			settings->prune_mode = it->value;
			return true;
		}
	}

	return false;
}

static bool parse_abort_conditions(struct settings *settings, const char *optarg)
{
	char *dup, *origdup, *p;
	if (!optarg)
		return set_abort_condition(settings, NULL);

	origdup = dup = strdup(optarg);
	while (dup) {
		if ((p = strchr(dup, ',')) != NULL) {
			*p = '\0';
			p++;
		}

		if (!set_abort_condition(settings, dup)) {
			free(origdup);
			return false;
		}

		dup = p;
	}

	free(origdup);

	return true;
}

static size_t char_to_multiplier(char c)
{
	switch (c) {
	case 'k':
	case 'K':
		return 1024UL;
	case 'm':
	case 'M':
		return 1024UL * 1024UL;
	case 'g':
	case 'G':
		return 1024UL * 1024UL * 1024UL;
	}

	return 0;
}

static bool parse_usage_limit(struct settings *settings, const char *optarg)
{
	size_t value;
	char *endptr = NULL;

	if (!optarg)
		return false;

	value = strtoul(optarg, &endptr, 10);

	if (*endptr) {
		size_t multiplier = char_to_multiplier(*endptr);

		if (multiplier == 0)
			return false;

		value *= multiplier;
	}

	settings->disk_usage_limit = value;
	return true;
}

static const char *usage_str =
	"usage: runner [options] [test_root] results-path\n"
	"   or: runner --list-all [options] [test_root]\n\n"
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
	"  --abort-on-monitored-error[=list]\n"
	"                        Abort execution when a fatal condition is detected.\n"
	"                        A comma-separated list of conditions to check can be\n"
	"                        given. If not given, all conditions are checked. An\n"
	"                        empty string as a condition disables aborting\n"
	"                        Possible conditions:\n"
	"                         lockdep - abort when kernel lockdep has been angered.\n"
	"                         taint   - abort when kernel becomes fatally tainted.\n"
	"                         ping    - abort when a host configured in .igtrc or\n"
	"                                   environment variable IGT_PING_HOSTNAME does\n"
	"                                   not respond to ping.\n"
	"                         all     - abort for all of the above.\n"
	"  -s, --sync            Sync results to disk after every test\n"
	"  -l {quiet,verbose,dummy}, --log-level {quiet,verbose,dummy}\n"
	"                        Set the logger verbosity level\n"
	"  --test-list TEST_LIST\n"
	"                        A file containing a list of tests to run\n"
	"  -o, --overwrite       If the results-path already exists, delete it\n"
	"  --ignore-missing      Ignored but accepted, for piglit compatibility\n"
	"\n"
	" Incompatible options:\n"
	"  --allow-non-root      Allow running tests without being the root user.\n"
	"  -m, --multiple-mode   Run multiple subtests in the same binary execution.\n"
	"                        If a testlist file is given, consecutive subtests are\n"
	"                        run in the same execution if they are from the same\n"
	"                        binary. Note that in that case relative ordering of the\n"
	"                        subtest execution is dictated by the test binary, not\n"
	"                        the testlist\n"
	"  --inactivity-timeout <seconds>\n"
	"                        Kill the running test after <seconds> of inactivity in\n"
	"                        the test's stdout, stderr, or dmesg\n"
	"  --per-test-timeout <seconds>\n"
	"                        Kill the running test after <seconds>. This timeout is per\n"
	"                        subtest, or dynamic subtest. In other words, every subtest,\n"
	"                        even when running in multiple-mode, must finish in <seconds>.\n"
	"  --overall-timeout <seconds>\n"
	"                        Don't execute more tests after <seconds> has elapsed\n"
	"  --disk-usage-limit <limit>\n"
	"                        Kill the running test if its logging, both itself and the\n"
	"                        kernel logs, exceed the given limit in bytes. The limit\n"
	"                        parameter can use suffixes k, M and G for kilo/mega/gigabytes,\n"
	"                        respectively. Limit of 0 (default) disables the limit.\n"
	"  --use-watchdog        Use hardware watchdog for lethal enforcement of the\n"
	"                        above timeout. Killing the test process is still\n"
	"                        attempted at timeout trigger.\n"
	"  --dmesg-warn-level <level>\n"
	"                        Messages with log level equal or lower (more serious)\n"
	"                        to the given one will override the test result to\n"
	"                        dmesg-warn/dmesg-fail, assuming they go through filtering.\n"
	"                        Defaults to 4 (KERN_WARNING).\n"
	"  --piglit-style-dmesg  Filter dmesg like piglit does. Piglit considers matches\n"
	"                        against a short filter list to mean the test result\n"
	"                        should be changed to dmesg-warn/dmesg-fail. Without\n"
	"                        this option everything except matches against a\n"
	"                        (longer) filter list means the test result should\n"
	"                        change. KERN_NOTICE dmesg level is treated as warn,\n"
	"                        unless overridden with --dmesg-warn-level.\n"
	"  --prune-mode <mode>   Control reporting of dynamic subtests by selecting test\n"
	"                        results that are removed from the final results set.\n"
	"                        Possible options:\n"
	"                         keep-dynamic-subtests  - Remove subtests that have dynamic\n"
	"                                                  subtests. (default)\n"
	"                         keep-dynamic           - Alias for the above\n"
	"                         keep-subtests          - Remove dynamic subtests,\n"
	"                                                  leaving just the parent subtest.\n"
	"                         keep-all               - Don't remove anything\n"
	"                         keep-requested         - Remove reported results that are\n"
	"                                                  not in the requested test set.\n"
	"                                                  Useful when you have a hand-written\n"
	"                                                  testlist.\n"
	"  -b, --blacklist FILENAME\n"
	"                        Exclude all test matching to regexes from FILENAME\n"
	"                        (can be used more than once)\n"
	"  -e, --environment <KEY or KEY=VALUE>\n"
	"                        Set an environment variable for the test process.\n"
	"                        If only the key is provided, the current value is read\n"
	"                        from the runner's environment (and saved for resumes).\n"
	"  -L, --list-all        List all matching subtests instead of running\n"
	"  --collect-code-cov    Enables gcov-based collect of code coverage for tests.\n"
	"                        Requires --collect-script FILENAME\n"
	"  --coverage-per-test   Stores code coverage results per each test.\n"
	"                        Requires --collect-script FILENAME\n"
	"  --collect-script FILENAME\n"
	"                        Use FILENAME as script to collect code coverage data.\n"
	"\n"
	"  [test_root]           Directory that contains the IGT tests. The environment\n"
	"                        variable IGT_TEST_ROOT will be used if set, overriding\n"
	"                        this option if given.\n"
	;

__attribute__ ((format (printf, 2, 3)))
static void usage(FILE *f, const char *extra_message, ...)
{
	if (extra_message) {
		va_list args;

		va_start(args, extra_message);
		vfprintf(f, extra_message, args);
		fputs("\n\n", f);
		va_end(args);
	}

	fputs(usage_str, f);
}

static bool add_regex(struct regex_list *list, char *new)
{
	GRegex *regex;
	GError *error = NULL;

	regex = g_regex_new(new, G_REGEX_OPTIMIZE, 0, &error);
	if (error) {
		usage(stderr, "Invalid regex '%s': %s", new, error->message);
		g_error_free(error);
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

static bool parse_blacklist(struct regex_list *exclude_regexes,
			    char *blacklist_filename)
{
	FILE *f;
	char *line = NULL;
	size_t line_len = 0;
	bool status = false;

	if ((f = fopen(blacklist_filename, "r")) == NULL) {
		fprintf(stderr, "Cannot open blacklist file %s\n", blacklist_filename);
		return false;
	}
	while (1) {
		size_t str_size = 0, idx = 0;

		if (getline(&line, &line_len, f) == -1) {
			if (errno == EINTR)
				continue;
			else
				break;
		}

		while (true) {
			if (line[idx] == '\n' ||
			    line[idx] == '\0' ||
			    line[idx] == '#')   /* # starts a comment */
				break;
			if (!isspace(line[idx]))
				str_size = idx + 1;
			idx++;
		}
		if (str_size > 0) {
			char *test_regex = strndup(line, str_size);

			status = add_regex(exclude_regexes, test_regex);
			if (!status)
				break;
		}
	}

	free(line);
	fclose(f);
	return status;
}

static void free_regexes(struct regex_list *regexes)
{
	size_t i;

	for (i = 0; i < regexes->size; i++) {
		free(regexes->regex_strings[i]);
		g_regex_unref(regexes->regexes[i]);
	}
	free(regexes->regex_strings);
	free(regexes->regexes);
}

/**
 * string_trim_and_duplicate() - returns a duplicated, trimmed string
 * @string: string to trim and duplicate
 * 
 * If the provided string is NULL, a NULL is returned. In any other case, a
 * newly-allocated string of length up to strlen(string) is returned. The
 * returned string has its whitespace removed (as detected by isspace), while
 * the original string is left unmodified.
 */
static char *string_trim_and_duplicate(const char *string) {
	size_t length;

	if (string == NULL)
		return NULL;

	length = strlen(string);

	while (length > 0 && isspace(string[0])) {
		string++;
		length--;
	}

	while (length > 0 && isspace(string[length - 1])) {
		length--;
	}

	return strndup(string, length);
}

/**
 * add_env_var() - Adds a new environment variable to the runner settings.
 * @env_vars: Pointer to the env var list head from the settings.
 * @key_value: Environment variable key-value pair string to add.
 *
 * key_value must be a string like "KEY=VALUE" or just "KEY" if the value is to
 * be loaded from the runner's environment variables. In the latter case, if
 * the requested variable is not set, the operation will fail.
 * 
 * An empty variable may be set by providing an key_value of "KEY=" or setting
 * an empty variable for the runner process, then providing just the "KEY".
 */
static bool add_env_var(struct igt_list_head *env_vars, const char *key_value) {
	char *env_kv, *value_str;
	struct environment_variable *var = NULL;

	if (env_vars == NULL || key_value == NULL || strlen(key_value) == 0)
		goto error;

	env_kv = strdup(key_value);
	value_str = strpbrk(env_kv, "\n=");

	if (value_str == env_kv) {
		fprintf(stderr, "Missing key for --environment \"%s\"\n", key_value);
		goto error;
	}

	if (value_str != NULL && value_str[0] != '=') {
		fprintf(stderr, "Invalid characters in key for --environment \"%s\"\n", key_value);
		goto error;
	}

	if (value_str != NULL) {
		/* value provided - copy string contents after '=' */
		value_str[0] = '\0';
		value_str++;
	} else {
		/* env_kv is the key - use the runner's environment, if set */
		value_str = getenv(env_kv);
		if (value_str == NULL) {
			fprintf(stderr, "No value provided for --environment \"%s\" and "
			                "variable is not set for igt_runner\n", env_kv);
			goto error;
		}
	}

	var = malloc(sizeof(struct environment_variable));

	var->key = string_trim_and_duplicate(env_kv);
	if (strlen(var->key) == 0) {
		fprintf(stderr, "Environment variable key is empty for \"%s\"\n", key_value);
		goto error;
	}

	var->value = strdup(value_str); /* Can be empty, that's okay */

	igt_list_add_tail(&var->link, env_vars);
	free(env_kv);
	return true;

error:
	if (var != NULL)
		free(var);

	if (env_kv != NULL)
		free(env_kv);

	return false;
}

static void free_env_vars(struct igt_list_head *env_vars) {
	struct environment_variable *iter;

	while (!igt_list_empty(env_vars)) {
		iter = igt_list_first_entry(env_vars, iter, link);

		free(iter->key);
		free(iter->value);

		igt_list_del(&iter->link);
		free(iter);
	}
}

static bool file_exists_at(int dirfd, const char *filename)
{
	return faccessat(dirfd, filename, F_OK, 0) == 0;
}

static bool readable_file(const char *filename)
{
	return !access(filename, R_OK);
}

static bool writeable_file(const char *filename)
{
	return !access(filename, W_OK);
}

static bool executable_file(const char *filename)
{
	return !access(filename, X_OK);
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

static char *bin_path(char *fname)
{
	char *path, *p;
	char file[PATH_MAX];

	if (strchr(fname, '/'))
		return absolute_path(fname);

	path = strdup(getenv("PATH"));
	p = strtok(path, ":");
	do {
		if (*p) {
			strcpy(file, p);
			strcat(file, "/");
			strcat(file, fname);
			if (executable_file(file)) {
				free(path);
				return strdup(file);
			}
		}
		p = strtok(NULL, ":");
	} while (p);

	free(path);
	return strdup(fname);
}

static void print_version(void)
{
	struct utsname uts;

	uname(&uts);

	printf("IGT-Version: %s-%s (%s) (%s: %s %s)\n", PACKAGE_VERSION,
	       IGT_GIT_SHA1, TARGET_CPU_PLATFORM,
	       uts.sysname, uts.release, uts.machine);
}

void init_settings(struct settings *settings)
{
	memset(settings, 0, sizeof(*settings));
	IGT_INIT_LIST_HEAD(&settings->env_vars);
}

void clear_settings(struct settings *settings)
{
	free(settings->test_list);
	free(settings->name);
	free(settings->test_root);
	free(settings->results_path);

	free_regexes(&settings->include_regexes);
	free_regexes(&settings->exclude_regexes);
	free_env_vars(&settings->env_vars);

	init_settings(settings);
}

bool parse_options(int argc, char **argv,
		   struct settings *settings)
{
	int c;
	char *env_test_root;

	static struct option long_options[] = {
		{"version", no_argument, NULL, OPT_VERSION},
		{"help", no_argument, NULL, OPT_HELP},
		{"name", required_argument, NULL, OPT_NAME},
		{"dry-run", no_argument, NULL, OPT_DRY_RUN},
		{"allow-non-root", no_argument, NULL, OPT_ALLOW_NON_ROOT},
		{"include-tests", required_argument, NULL, OPT_INCLUDE},
		{"exclude-tests", required_argument, NULL, OPT_EXCLUDE},
		{"environment", required_argument, NULL, OPT_ENVIRONMENT},
		{"abort-on-monitored-error", optional_argument, NULL, OPT_ABORT_ON_ERROR},
		{"disk-usage-limit", required_argument, NULL, OPT_DISK_USAGE_LIMIT},
		{"sync", no_argument, NULL, OPT_SYNC},
		{"log-level", required_argument, NULL, OPT_LOG_LEVEL},
		{"test-list", required_argument, NULL, OPT_TEST_LIST},
		{"overwrite", no_argument, NULL, OPT_OVERWRITE},
		{"ignore-missing", no_argument, NULL, OPT_IGNORE_MISSING},
		{"collect-code-cov", no_argument, NULL, OPT_ENABLE_CODE_COVERAGE},
		{"coverage-per-test", no_argument, NULL, OPT_COV_RESULTS_PER_TEST},
		{"collect-script", required_argument, NULL, OPT_CODE_COV_SCRIPT},
		{"multiple-mode", no_argument, NULL, OPT_MULTIPLE},
		{"inactivity-timeout", required_argument, NULL, OPT_TIMEOUT},
		{"per-test-timeout", required_argument, NULL, OPT_PER_TEST_TIMEOUT},
		{"overall-timeout", required_argument, NULL, OPT_OVERALL_TIMEOUT},
		{"use-watchdog", no_argument, NULL, OPT_WATCHDOG},
		{"piglit-style-dmesg", no_argument, NULL, OPT_PIGLIT_DMESG},
		{"dmesg-warn-level", required_argument, NULL, OPT_DMESG_WARN_LEVEL},
		{"prune-mode", required_argument, NULL, OPT_PRUNE_MODE},
		{"blacklist", required_argument, NULL, OPT_BLACKLIST},
		{"list-all", no_argument, NULL, OPT_LIST_ALL},
		{ 0, 0, 0, 0},
	};

	clear_settings(settings);

	optind = 1;

	settings->dmesg_warn_level = -1;

	while ((c = getopt_long(argc, argv, "hn:dt:x:e:sl:omb:L",
				long_options, NULL)) != -1) {
		switch (c) {
		case OPT_VERSION:
			print_version();
			goto error;
		case OPT_HELP:
			usage(stdout, NULL);
			goto error;
		case OPT_NAME:
			settings->name = strdup(optarg);
			break;
		case OPT_DRY_RUN:
			settings->dry_run = true;
			break;
		case OPT_ALLOW_NON_ROOT:
			settings->allow_non_root = true;
			break;
		case OPT_INCLUDE:
			if (!add_regex(&settings->include_regexes, strdup(optarg)))
				goto error;
			break;
		case OPT_EXCLUDE:
			if (!add_regex(&settings->exclude_regexes, strdup(optarg)))
				goto error;
			break;
		case OPT_ENVIRONMENT:
			if (!add_env_var(&settings->env_vars, optarg))
				goto error;
			break;
		case OPT_ABORT_ON_ERROR:
			if (!parse_abort_conditions(settings, optarg))
				goto error;
			break;
		case OPT_DISK_USAGE_LIMIT:
			if (!parse_usage_limit(settings, optarg)) {
				usage(stderr, "Cannot parse disk usage limit");
				goto error;
			}
			break;
		case OPT_SYNC:
			settings->sync = true;
			break;
		case OPT_LOG_LEVEL:
			if (!set_log_level(settings, optarg)) {
				usage(stderr, "Cannot parse log level");
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
		case OPT_ENABLE_CODE_COVERAGE:
			settings->enable_code_coverage = true;
			break;
		case OPT_COV_RESULTS_PER_TEST:
			settings->cov_results_per_test = true;
			break;
		case OPT_CODE_COV_SCRIPT:
			settings->code_coverage_script = bin_path(optarg);
			break;

		case OPT_MULTIPLE:
			settings->multiple_mode = true;
			break;
		case OPT_TIMEOUT:
			settings->inactivity_timeout = atoi(optarg);
			break;
		case OPT_PER_TEST_TIMEOUT:
			settings->per_test_timeout = atoi(optarg);
			break;
		case OPT_OVERALL_TIMEOUT:
			settings->overall_timeout = atoi(optarg);
			break;
		case OPT_WATCHDOG:
			settings->use_watchdog = true;
			break;
		case OPT_PIGLIT_DMESG:
			settings->piglit_style_dmesg = true;
			if (settings->dmesg_warn_level < 0)
				settings->dmesg_warn_level = 5; /* KERN_NOTICE */
			break;
		case OPT_DMESG_WARN_LEVEL:
			settings->dmesg_warn_level = atoi(optarg);
			break;
		case OPT_PRUNE_MODE:
			if (!set_prune_mode(settings, optarg)) {
				usage(stderr, "Cannot parse prune mode");
				goto error;
			}
			break;
		case OPT_BLACKLIST:
			if (!parse_blacklist(&settings->exclude_regexes,
					     absolute_path(optarg)))
				goto error;
			break;
		case OPT_LIST_ALL:
			settings->list_all = true;
			break;
		case '?':
			usage(stderr, NULL);
			goto error;
		default:
			usage(stderr, "Cannot parse options");
			goto error;
		}
	}

	if (settings->dmesg_warn_level < 0)
		settings->dmesg_warn_level = 4; /* KERN_WARN */

	if (settings->list_all) { /* --list-all doesn't require results path */
		switch (argc - optind) {
		case 1:
			settings->test_root = absolute_path(argv[optind]);
			++optind;
			/* fallthrough */
		case 0:
			break;
		default:
			usage(stderr, "Too many arguments for --list-all");
			goto error;
		}
	} else {
		switch (argc - optind) {
		case 2:
			settings->test_root = absolute_path(argv[optind]);
			++optind;
			/* fallthrough */
		case 1:
			settings->results_path = absolute_path(argv[optind]);
			break;
		case 0:
			usage(stderr, "Results-path missing");
			goto error;
		default:
			usage(stderr, "Extra arguments after results-path");
			goto error;
		}
		if (!settings->name) {
			char *name = strdup(settings->results_path);

			settings->name = strdup(basename(name));
			free(name);
		}
	}

	if ((env_test_root = getenv("IGT_TEST_ROOT")) != NULL) {
		free(settings->test_root);
		settings->test_root = absolute_path(env_test_root);
	}

	if (!settings->test_root) {
		usage(stderr, "Test root not set");
		goto error;
	}


	return true;

 error:
	clear_settings(settings);
	return false;
}

bool validate_settings(struct settings *settings)
{
	int dirfd, fd;

	if (settings->test_list && !readable_file(settings->test_list)) {
		usage(stderr, "Cannot open test-list file");
		return false;
	}

	if (!settings->results_path) {
		usage(stderr, "No results-path set; this shouldn't happen");
		return false;
	}

	if (!settings->test_root) {
		usage(stderr, "No test root set; this shouldn't happen");
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

	/* enables code coverage when --coverage-per-test is used */
	if (settings->cov_results_per_test)
		settings->enable_code_coverage = true;

	if (!settings->allow_non_root && (getuid() != 0)) {
		fprintf(stderr, "Runner needs to run with UID 0 (root).\n");
		return false;
	}

	if (settings->enable_code_coverage) {
		if (!executable_file(settings->code_coverage_script)) {
			fprintf(stderr, "%s doesn't exist or is not executable\n", settings->code_coverage_script);
			return false;
		}
		if (!writeable_file(GCOV_RESET)) {
			if (getuid() != 0)
				fprintf(stderr, "Code coverage requires root.\n");
			else
				fprintf(stderr, "Is GCOV enabled? Can't access %s stat.\n", GCOV_RESET);
			return false;
		}
	}

	return true;
}

/**
 * fopenat() - wrapper for fdopen(openat(dirfd, filename))
 * @dirfd: directory fd to pass to openat
 * @filename: file name to open with openat
 * @flags: flags to pass to openat
 * @openat_mode: (octal) mode to pass to openat
 * @fdopen_mode: (text) mode to pass to fdopen
 */
static FILE *fopenat(int dirfd, const char *filename, int flags, mode_t openat_mode, const char *fdopen_mode)
{
	int fd;
	FILE *f;

	if ((fd = openat(dirfd, filename, flags, openat_mode)) < 0) {
		usage(stderr, "fopenat(%s, %d) failed: %s",
		      filename, flags, strerror(errno));
		return NULL;
	}

	f = fdopen(fd, fdopen_mode);
	if (!f) {
		close(fd);
		return NULL;
	}

	return f;
}

static FILE *fopenat_read(int dirfd, const char *filename)
{
	return fopenat(dirfd, filename, O_RDONLY, 0000, "r");
}

static FILE *fopenat_create(int dirfd, const char *filename, bool overwrite)
{
	mode_t mode_if_exists = overwrite ? O_TRUNC : O_EXCL;
	return fopenat(dirfd, filename, O_CREAT | O_RDWR | mode_if_exists, 0666, "w");
}

static bool serialize_environment(struct settings *settings, int dirfd)
{
	struct environment_variable *iter;
	FILE *f;

	if (file_exists_at(dirfd, env_filename) && !settings->overwrite) {
		usage(stderr, "%s already exists, not overwriting", env_filename);
		return false;
	}

	if ((f = fopenat_create(dirfd, env_filename, settings->overwrite)) == NULL)
		return false;

	igt_list_for_each_entry(iter, &settings->env_vars, link) {
		fprintf(f, "%s=%s\n", iter->key, iter->value);
	}

	if (settings->sync) {
		fflush(f);
		fsync(fileno(f));
	}

	fclose(f);
	return true;
}

bool serialize_settings(struct settings *settings)
{
#define SERIALIZE_LINE(f, s, name, format) fprintf(f, "%s : " format "\n", #name, s->name)

	FILE *f;
	int dirfd, covfd;
	char path[PATH_MAX];

	if (!settings->results_path) {
		usage(stderr, "No results-path set; this shouldn't happen");
		return false;
	}

	if ((dirfd = open(settings->results_path, O_DIRECTORY | O_RDONLY)) < 0) {
		mkdir(settings->results_path, 0755);
		if ((dirfd = open(settings->results_path, O_DIRECTORY | O_RDONLY)) < 0) {
			usage(stderr, "Creating results-path failed");
			return false;
		}
	}

	if (settings->enable_code_coverage) {
		strcpy(path, settings->results_path);
		strcat(path, CODE_COV_RESULTS_PATH);
		if ((covfd = open(path, O_DIRECTORY | O_RDONLY)) < 0) {
			if (mkdir(path, 0755)) {
				usage(stderr, "Creating code coverage path failed");
				return false;
			}
		} else {
			close(covfd);
		}
	}

	if (file_exists_at(dirfd, settings_filename) && !settings->overwrite) {
		usage(stderr, "%s already exists, not overwriting", settings_filename);
		close(dirfd);
		return false;
	}

	if ((f = fopenat_create(dirfd, settings_filename, settings->overwrite)) == NULL) {
		close(dirfd);
		return false;
	}

	SERIALIZE_LINE(f, settings, abort_mask, "%d");
	SERIALIZE_LINE(f, settings, disk_usage_limit, "%zd");
	if (settings->test_list)
		SERIALIZE_LINE(f, settings, test_list, "%s");
	if (settings->name)
		SERIALIZE_LINE(f, settings, name, "%s");
	SERIALIZE_LINE(f, settings, dry_run, "%d");
	SERIALIZE_LINE(f, settings, allow_non_root, "%d");
	SERIALIZE_LINE(f, settings, sync, "%d");
	SERIALIZE_LINE(f, settings, log_level, "%d");
	SERIALIZE_LINE(f, settings, overwrite, "%d");
	SERIALIZE_LINE(f, settings, multiple_mode, "%d");
	SERIALIZE_LINE(f, settings, inactivity_timeout, "%d");
	SERIALIZE_LINE(f, settings, per_test_timeout, "%d");
	SERIALIZE_LINE(f, settings, overall_timeout, "%d");
	SERIALIZE_LINE(f, settings, use_watchdog, "%d");
	SERIALIZE_LINE(f, settings, piglit_style_dmesg, "%d");
	SERIALIZE_LINE(f, settings, dmesg_warn_level, "%d");
	SERIALIZE_LINE(f, settings, prune_mode, "%d");
	SERIALIZE_LINE(f, settings, test_root, "%s");
	SERIALIZE_LINE(f, settings, results_path, "%s");
	SERIALIZE_LINE(f, settings, enable_code_coverage, "%d");
	SERIALIZE_LINE(f, settings, cov_results_per_test, "%d");
	SERIALIZE_LINE(f, settings, code_coverage_script, "%s");

	if (settings->sync) {
		fflush(f);
		fsync(fileno(f));
	}

	fclose(f);

	if (!igt_list_empty(&settings->env_vars)) {
		if (!serialize_environment(settings, dirfd)) {
			close(dirfd);
			return false;
		}
	}

	if (settings->sync)
		fsync(dirfd);

	close(dirfd);
	return true;

#undef SERIALIZE_LINE
}

bool read_settings_from_file(struct settings *settings, FILE *f)
{
#define PARSE_LINE(s, name, val, field, write) \
	if (!strcmp(name, #field)) {	       \
		s->field = write;	       \
		free(name);		       \
		free(val);		       \
		name = val = NULL;	       \
		continue;		       \
	}

	char *name = NULL, *val = NULL;

	settings->dmesg_warn_level = -1;

	while (fscanf(f, "%ms : %m[^\n]", &name, &val) == 2) {
		int numval = atoi(val);
		PARSE_LINE(settings, name, val, abort_mask, numval);
		PARSE_LINE(settings, name, val, disk_usage_limit, strtoul(val, NULL, 10));
		PARSE_LINE(settings, name, val, test_list, val ? strdup(val) : NULL);
		PARSE_LINE(settings, name, val, name, val ? strdup(val) : NULL);
		PARSE_LINE(settings, name, val, dry_run, numval);
		PARSE_LINE(settings, name, val, allow_non_root, numval);
		PARSE_LINE(settings, name, val, sync, numval);
		PARSE_LINE(settings, name, val, log_level, numval);
		PARSE_LINE(settings, name, val, overwrite, numval);
		PARSE_LINE(settings, name, val, multiple_mode, numval);
		PARSE_LINE(settings, name, val, inactivity_timeout, numval);
		PARSE_LINE(settings, name, val, per_test_timeout, numval);
		PARSE_LINE(settings, name, val, overall_timeout, numval);
		PARSE_LINE(settings, name, val, use_watchdog, numval);
		PARSE_LINE(settings, name, val, piglit_style_dmesg, numval);
		PARSE_LINE(settings, name, val, dmesg_warn_level, numval);
		PARSE_LINE(settings, name, val, prune_mode, numval);
		PARSE_LINE(settings, name, val, test_root, val ? strdup(val) : NULL);
		PARSE_LINE(settings, name, val, results_path, val ? strdup(val) : NULL);
		PARSE_LINE(settings, name, val, enable_code_coverage, numval);
		PARSE_LINE(settings, name, val, cov_results_per_test, numval);
		PARSE_LINE(settings, name, val, code_coverage_script, val ? strdup(val) : NULL);

		printf("Warning: Unknown field in settings file: %s = %s\n",
		       name, val);
		free(name);
		free(val);
		name = val = NULL;
	}

	if (settings->dmesg_warn_level < 0) {
		if (settings->piglit_style_dmesg)
			settings->dmesg_warn_level = 5;
		else
			settings->dmesg_warn_level = 4;
	}

	free(name);
	free(val);

	return true;

#undef PARSE_LINE
}

/**
 * read_env_vars_from_file() - load env vars from a file
 * @env_vars: list head to add env var entries to
 * @f: opened file stream to read
 *
 * Loads the environment.txt file and adds each line-separated KEY=VALUE pair
 * into the provided env_vars list. Lines not containing the '=' K-V separator,
 * starting with a '#' (comments) or '=' (missing keys) are ignored.
 *
 * The function returns true if the env vars were read successfully, or there
 * was nothing to be read.
 */
static bool read_env_vars_from_file(struct igt_list_head *env_vars, FILE *f)
{
	char *line = NULL;
	ssize_t line_length;
	size_t line_buffer_length = 0;

	while ((line_length = getline(&line, &line_buffer_length, f)) != -1) {
		char *line_ptr = line;

		while (isspace(*line_ptr)) {
			line_length--;
			line_ptr++;
		}

		if (*line_ptr == '\0' || *line_ptr == '#' || *line_ptr == '=')
			continue; /* Empty, whitespace, comment or missing key */

		if (strchr(line_ptr, '=') == NULL)
			continue; /* Missing value */

		/* Strip the line feed, but keep trailing whitespace */
		if (line_length > 0 && line[line_length - 1] == '\n')
			line[line_length - 1] = '\0';

		add_env_var(env_vars, line);
	}

	/* input file can be empty */
	if (line != NULL)
		free(line);

	return true;
}

bool read_settings_from_dir(struct settings *settings, int dirfd)
{
	FILE *f;

	clear_settings(settings);

	/* settings are always there */
	if ((f = fopenat_read(dirfd, settings_filename)) == NULL)
		return false;

	if (!read_settings_from_file(settings, f)) {
		fclose(f);
		return false;
	}

	fclose(f);

	/* env file may not exist if no --environment was set */
	if (file_exists_at(dirfd, env_filename)) {
		if ((f = fopenat_read(dirfd, env_filename)) == NULL)
			return false;

		if (!read_env_vars_from_file(&settings->env_vars, f)) {
			fclose(f);
			return false;
		}

		fclose(f);
	}

	return true;
}
