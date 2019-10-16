#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <json.h>

#include "igt_aux.h"
#include "igt_core.h"
#include "resultgen.h"
#include "settings.h"
#include "executor.h"
#include "output_strings.h"

#define INCOMPLETE_EXITCODE -1

_Static_assert(INCOMPLETE_EXITCODE != IGT_EXIT_SKIP, "exit code clash");
_Static_assert(INCOMPLETE_EXITCODE != IGT_EXIT_SUCCESS, "exit code clash");
_Static_assert(INCOMPLETE_EXITCODE != IGT_EXIT_INVALID, "exit code clash");

struct subtests
{
	char **names;
	size_t size;
};

struct results
{
	struct json_object *tests;
	struct json_object *totals;
	struct json_object *runtimes;
};

/*
 * A lot of string handling here operates on an mmapped buffer, and
 * thus we can't assume null-terminated strings. Buffers will be
 * passed around as pointer+size, or pointer+pointer-past-the-end, the
 * mem*() family of functions is used instead of str*().
 */

static char *find_line_starting_with(char *haystack, const char *needle, char *end)
{
	while (haystack < end) {
		char *line_end = memchr(haystack, '\n', end - haystack);

		if (end - haystack < strlen(needle))
			return NULL;
		if (!memcmp(haystack, needle, strlen(needle)))
			return haystack;
		if (line_end == NULL)
			return NULL;
		haystack = line_end + 1;
	}

	return NULL;
}

static const char *next_line(const char *line, const char *bufend)
{
	char *ret;

	if (!line)
		return NULL;

	ret = memchr(line, '\n', bufend - line);
	if (ret)
		ret++;

	if (ret < bufend)
		return ret;
	else
		return NULL;
}

static size_t count_lines(const char *buf, const char *bufend)
{
	size_t ret = 0;
	while (buf < bufend && (buf = memchr(buf, '\n', bufend - buf)) != NULL) {
		ret++;
		buf++;
	}

	return ret;
}

static void append_line(char **buf, size_t *buflen, char *line)
{
	size_t linelen = strlen(line);

	*buf = realloc(*buf, *buflen + linelen + 1);
	strcpy(*buf + *buflen, line);
	*buflen += linelen;
}

static const struct {
	const char *output_str;
	const char *result_str;
} resultmap[] = {
	{ "SUCCESS", "pass" },
	{ "SKIP", "skip" },
	{ "FAIL", "fail" },
	{ "CRASH", "crash" },
	{ "TIMEOUT", "timeout" },
};
static void parse_result_string(const char *resultstring, size_t len, const char **result, double *time)
{
	size_t i;
	size_t wordlen = 0;

	while (wordlen < len && !isspace(resultstring[wordlen])) {
		wordlen++;
	}

	*result = NULL;
	for (i = 0; i < (sizeof(resultmap) / sizeof(resultmap[0])); i++) {
		if (!strncmp(resultstring, resultmap[i].output_str, wordlen)) {
			*result = resultmap[i].result_str;
			break;
		}
	}

	/* If the result string is unknown, use incomplete */
	if (!*result)
		*result = "incomplete";

	/*
	 * Check for subtest runtime after the result. The string is
	 * '(' followed by the runtime in seconds as floating point,
	 * followed by 's)'.
	 */
	wordlen++;
	if (wordlen < len && resultstring[wordlen] == '(') {
		char *dup;

		wordlen++;
		dup = malloc(len - wordlen + 1);
		memcpy(dup, resultstring + wordlen, len - wordlen);
		dup[len - wordlen] = '\0';
		*time = strtod(dup, NULL);

		free(dup);
	}
}

static void parse_subtest_result(const char *subtest,
				 const char **result,
				 double *time,
				 const char *line,
				 const char *bufend)
{
	const char *resultstring;
	size_t subtestlen = strlen(subtest);
	const char *line_end;
	size_t linelen;

	*result = "incomplete";
	*time = 0.0;

	/*
	 * The result line structure is:
	 *
	 * - The string "Subtest " (`SUBTEST_RESULT` from output_strings.h)
	 * - The subtest name
	 * - The characters ':' and ' '
	 * - Subtest result string
	 * - Optional:
	 * -- The characters ' ' and '('
	 * -- Subtest runtime in seconds as floating point
	 * -- The characters 's' and ')'
	 *
	 * Example:
	 * Subtest subtestname: PASS (0.003s)
	 */

	if (!line)
		return;

	line_end = memchr(line, '\n', bufend - line);
	linelen = line_end != NULL ? line_end - line : bufend - line;

	if (strlen(SUBTEST_RESULT) + subtestlen + strlen(": ") > linelen ||
	    strncmp(line + strlen(SUBTEST_RESULT), subtest, subtestlen)) {
		/* This is not the correct result line */
		return;
	}

	resultstring = line + strlen(SUBTEST_RESULT) + subtestlen + strlen(": ");
	parse_result_string(resultstring, linelen - (resultstring - line), result, time);
}

static struct json_object *get_or_create_json_object(struct json_object *base,
						     const char *key)
{
	struct json_object *ret;

	if (json_object_object_get_ex(base, key, &ret))
		return ret;

	ret = json_object_new_object();
	json_object_object_add(base, key, ret);

	return ret;
}

static void set_result(struct json_object *obj, const char *result)
{
	if (result)
		json_object_object_add(obj, "result",
				       json_object_new_string(result));
}

static void add_runtime(struct json_object *obj, double time)
{
	double oldtime;
	struct json_object *timeobj = get_or_create_json_object(obj, "time");
	struct json_object *oldend;

	json_object_object_add(timeobj, "__type__",
			       json_object_new_string("TimeAttribute"));
	json_object_object_add(timeobj, "start",
			       json_object_new_double(0.0));

	if (!json_object_object_get_ex(timeobj, "end", &oldend)) {
		json_object_object_add(timeobj, "end",
				       json_object_new_double(time));
		return;
	}

	/* Add the runtime to the existing runtime. */
	oldtime = json_object_get_double(oldend);
	time += oldtime;
	json_object_object_add(timeobj, "end",
			       json_object_new_double(time));
}

static void set_runtime(struct json_object *obj, double time)
{
	struct json_object *timeobj = get_or_create_json_object(obj, "time");

	json_object_object_add(timeobj, "__type__",
			       json_object_new_string("TimeAttribute"));
	json_object_object_add(timeobj, "start",
			       json_object_new_double(0.0));
	json_object_object_add(timeobj, "end",
			       json_object_new_double(time));
}

struct match_item
{
	const char *where;
	const char *what;
};

struct matches
{
	struct match_item *items;
	size_t size;
};

static void match_add(struct matches *matches, const char *where, const char *what)
{
	struct match_item newitem = { where, what };

	matches->size++;
	matches->items = realloc(matches->items, matches->size * sizeof(*matches->items));
	matches->items[matches->size - 1] = newitem;
}

static struct matches find_matches(const char *buf, const char *bufend,
				   const char **needles)
{
	struct matches ret = {};

	while (buf < bufend) {
		const char **needle;

		for (needle = needles; *needle; needle++) {
			if (bufend - buf < strlen(*needle))
				continue;

			if (!memcmp(buf, *needle, strlen(*needle))) {
				match_add(&ret, buf, *needle);
				goto end_find;
			}
		}

	end_find:
		buf = next_line(buf, bufend);
		if (!buf)
			break;
	}

	return ret;
}

static void free_matches(struct matches *matches)
{
	free(matches->items);
}

static bool fill_from_output(int fd, const char *binary, const char *key,
			     struct subtests *subtests,
			     struct json_object *tests)
{
	char *buf, *bufend, *nullchr;
	struct stat statbuf;
	char piglit_name[256];
	char *igt_version = NULL;
	size_t igt_version_len = 0;
	struct json_object *current_test = NULL;
	const char *needles[] = {
		STARTING_SUBTEST,
		SUBTEST_RESULT,
		NULL
	};
	struct matches matches = {};
	size_t i, k;

	if (fstat(fd, &statbuf))
		return false;

	if (statbuf.st_size != 0) {
		buf = mmap(NULL, statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
		if (buf == MAP_FAILED)
			return false;
	} else {
		buf = NULL;
	}

	/*
	 * Avoid null characters: Just pretend the output stops at the
	 * first such character, if any.
	 */
	if ((nullchr = memchr(buf, '\0', statbuf.st_size)) != NULL) {
		statbuf.st_size = nullchr - buf;
	}

	bufend = buf + statbuf.st_size;

	igt_version = find_line_starting_with(buf, IGT_VERSIONSTRING, bufend);
	if (igt_version) {
		char *newline = memchr(igt_version, '\n', bufend - igt_version);
		igt_version_len = newline - igt_version;
	}

	/* TODO: Refactor to helper functions */
	if (subtests->size == 0) {
		/* No subtests */
		generate_piglit_name(binary, NULL, piglit_name, sizeof(piglit_name));
		current_test = get_or_create_json_object(tests, piglit_name);

		json_object_object_add(current_test, key,
				       json_object_new_string_len(buf, statbuf.st_size));
		if (igt_version)
			json_object_object_add(current_test, "igt-version",
					       json_object_new_string_len(igt_version,
									  igt_version_len));

		return true;
	}

	matches = find_matches(buf, bufend, needles);

	for (i = 0; i < subtests->size; i++) {
		char *this_sub_begin, *this_sub_result;
		int begin_idx = -1, result_idx = -1;
		const char *resulttext;
		const char *beg, *end;
		double time;
		int begin_len;
		int result_len;

		generate_piglit_name(binary, subtests->names[i], piglit_name, sizeof(piglit_name));
		current_test = get_or_create_json_object(tests, piglit_name);

		begin_len = asprintf(&this_sub_begin, "%s%s\n", STARTING_SUBTEST, subtests->names[i]);
		result_len = asprintf(&this_sub_result, "%s%s: ", SUBTEST_RESULT, subtests->names[i]);

		if (begin_len < 0 || result_len < 0) {
			fprintf(stderr, "Failure generating strings\n");
			return false;
		}

		for (k = 0; k < matches.size; k++) {
			if (matches.items[k].what == STARTING_SUBTEST &&
			    !memcmp(matches.items[k].where,
				    this_sub_begin,
				    min(begin_len, bufend - matches.items[k].where))) {
				beg = matches.items[k].where;
				begin_idx = k;
			}
			if (matches.items[k].what == SUBTEST_RESULT &&
			    !memcmp(matches.items[k].where,
				    this_sub_result,
				    min(result_len, bufend - matches.items[k].where))) {
				end = matches.items[k].where;
				result_idx = k;
			}
		}

		free(this_sub_begin);
		free(this_sub_result);

		if (begin_idx < 0 && result_idx < 0) {
			/* No output at all */
			beg = buf;
			end = bufend;
		} else if (begin_idx < 0) {
			/*
			 * Subtest didn't start, but we have the
			 * result. Probably because an igt_fixture
			 * made it fail/skip.
			 *
			 * Start the output right after the previous
			 * subtest result/start.
			 */
			if (result_idx > 0)
				beg = next_line(matches.items[result_idx - 1].where, bufend);
			else
				beg = buf;
		} else {
			/* Stretch output beginning backwards */
			if (begin_idx == 0)
				beg = buf;
			else
				beg = next_line(matches.items[begin_idx - 1].where, bufend);
		}

		if (result_idx < 0 && begin_idx < 0) {
			/* No output at all: Already handled above */
		} else if (result_idx < 0) {
			/*
			 * Incomplete result. Include output up to the
			 * next starting subtest or result.
			 */
			if (begin_idx < matches.size - 1)
				end = matches.items[begin_idx + 1].where;
			else
				end = bufend;
		} else {
			/* Stretch output to the next starting subtest or result. */
			if (result_idx < matches.size - 1)
				end = matches.items[result_idx + 1].where;
			else
				end = bufend;
		}

		json_object_object_add(current_test, key,
				       json_object_new_string_len(beg, end - beg));

		if (igt_version) {
			json_object_object_add(current_test, "igt-version",
					       json_object_new_string_len(igt_version,
									  igt_version_len));
		}

		if (!json_object_object_get_ex(current_test, "result", NULL)) {
			parse_subtest_result(subtests->names[i],
					     &resulttext, &time,
					     result_idx < 0 ? NULL : matches.items[result_idx].where,
					     end);
			set_result(current_test, resulttext);
			set_runtime(current_test, time);
		}
	}

	free_matches(&matches);
	return true;
}

/*
 * This regexp controls the kmsg handling. All kernel log records that
 * have log level of warning or higher convert the result to
 * dmesg-warn/dmesg-fail unless they match this regexp.
 *
 * TODO: Move this to external files, i915-suppressions.txt,
 * general-suppressions.txt et al.
 */

#define _ "|"
static const char igt_dmesg_whitelist[] =
	"ACPI: button: The lid device is not compliant to SW_LID" _
	"ACPI: .*: Unable to dock!" _
	"IRQ [0-9]+: no longer affine to CPU[0-9]+" _
	"IRQ fixup: irq [0-9]+ move in progress, old vector [0-9]+" _
	/* i915 tests set module options, expected message */
	"Setting dangerous option [a-z_]+ - tainting kernel" _
	/* Raw printk() call, uses default log level (warn) */
	"Suspending console\\(s\\) \\(use no_console_suspend to debug\\)" _
	"atkbd serio[0-9]+: Failed to (deactivate|enable) keyboard on isa[0-9]+/serio[0-9]+" _
	"cache: parent cpu[0-9]+ should not be sleeping" _
	"hpet[0-9]+: lost [0-9]+ rtc interrupts" _
	/* i915 selftests terminate normally with ENODEV from the
	 * module load after the testing finishes, which produces this
	 * message.
	 */
	"i915: probe of [0-9:.]+ failed with error -25" _
	/* swiotbl warns even when asked not to */
	"mock: DMA: Out of SW-IOMMU space for [0-9]+ bytes" _
	"usb usb[0-9]+: root hub lost power or was reset"
	;
#undef _

static const char igt_piglit_style_dmesg_blacklist[] =
	"(\\[drm:|drm_|intel_|i915_)";

static bool init_regex_whitelist(struct settings* settings, GRegex **re)
{
	GError *err = NULL;
	const char *regex = settings->piglit_style_dmesg ?
		igt_piglit_style_dmesg_blacklist :
		igt_dmesg_whitelist;

	*re = g_regex_new(regex, G_REGEX_OPTIMIZE, 0, &err);
	if (err) {
		fprintf(stderr, "Cannot compile dmesg regexp\n");
		g_error_free(err);
		return false;
	}

	return true;
}

static bool parse_dmesg_line(char* line,
			     unsigned *flags, unsigned long long *ts_usec,
			     char *continuation, char **message)
{
	unsigned long long seq;
	int s;

	s = sscanf(line, "%u,%llu,%llu,%c;", flags, &seq, ts_usec, continuation);
	if (s != 4) {
		/*
		 * Machine readable key/value pairs begin with
		 * a space. We ignore them.
		 */
		if (line[0] != ' ') {
			fprintf(stderr, "Cannot parse kmsg record: %s\n", line);
		}
		return false;
	}

	*message = strchr(line, ';');
	if (!message) {
		fprintf(stderr, "No ; found in kmsg record, this shouldn't happen\n");
		return false;
	}
	(*message)++;

	return true;
}

static void generate_formatted_dmesg_line(char *message,
					  unsigned flags,
					  unsigned long long ts_usec,
					  char **formatted)
{
	char prefix[512];
	size_t messagelen;
	size_t prefixlen;
	char *p, *f;

	snprintf(prefix, sizeof(prefix),
		 "<%u> [%llu.%06llu] ",
		 flags & 0x07,
		 ts_usec / 1000000,
		 ts_usec % 1000000);

	messagelen = strlen(message);
	prefixlen = strlen(prefix);

	/*
	 * Decoding the hex escapes only makes the string shorter, so
	 * we can use the original length
	 */
	*formatted = malloc(strlen(prefix) + messagelen + 1);
	strcpy(*formatted, prefix);

	f = *formatted + prefixlen;
	for (p = message; *p; p++, f++) {
		if (p - message + 4 < messagelen &&
		    p[0] == '\\' && p[1] == 'x') {
			int c = 0;
			/* newline and tab are not isprint(), but they are isspace() */
			if (sscanf(p, "\\x%2x", &c) == 1 &&
			    (isprint(c) || isspace(c))) {
				*f = c;
				p += 3;
				continue;
			}
		}
		*f = *p;
	}
	*f = '\0';
}

static void add_dmesg(struct json_object *obj,
		      const char *dmesg, size_t dmesglen,
		      const char *warnings, size_t warningslen)
{
	json_object_object_add(obj, "dmesg",
			       json_object_new_string_len(dmesg, dmesglen));

	if (warnings) {
		json_object_object_add(obj, "dmesg-warnings",
				       json_object_new_string_len(warnings, warningslen));
	}
}

static void add_empty_dmesgs_where_missing(struct json_object *tests,
					   char *binary,
					   struct subtests *subtests)
{
	struct json_object *current_test;
	char piglit_name[256];
	size_t i;

	for (i = 0; i < subtests->size; i++) {
		generate_piglit_name(binary, subtests->names[i], piglit_name, sizeof(piglit_name));
		current_test = get_or_create_json_object(tests, piglit_name);
		if (!json_object_object_get_ex(current_test, "dmesg", NULL)) {
			add_dmesg(current_test, "", 0, NULL, 0);
		}
	}

}

static bool fill_from_dmesg(int fd,
			    struct settings *settings,
			    char *binary,
			    struct subtests *subtests,
			    struct json_object *tests)
{
	char *line = NULL, *warnings = NULL, *dmesg = NULL;
	size_t linelen = 0, warningslen = 0, dmesglen = 0;
	struct json_object *current_test = NULL;
	FILE *f = fdopen(fd, "r");
	char piglit_name[256];
	ssize_t read;
	size_t i;
	GRegex *re;

	if (!f) {
		return false;
	}

	if (!init_regex_whitelist(settings, &re)) {
		fclose(f);
		return false;
	}

	while ((read = getline(&line, &linelen, f)) > 0) {
		char *formatted;
		unsigned flags;
		unsigned long long ts_usec;
		char continuation;
		char *message, *subtest;

		if (!parse_dmesg_line(line, &flags, &ts_usec, &continuation, &message))
			continue;

		generate_formatted_dmesg_line(message, flags, ts_usec, &formatted);

		if ((subtest = strstr(message, STARTING_SUBTEST_DMESG)) != NULL) {
			if (current_test != NULL) {
				/* Done with the previous subtest, file up */
				add_dmesg(current_test, dmesg, dmesglen, warnings, warningslen);

				free(dmesg);
				free(warnings);
				dmesg = warnings = NULL;
				dmesglen = warningslen = 0;
			}

			subtest += strlen(STARTING_SUBTEST_DMESG);
			generate_piglit_name(binary, subtest, piglit_name, sizeof(piglit_name));
			current_test = get_or_create_json_object(tests, piglit_name);
		}

		if (settings->piglit_style_dmesg) {
			if ((flags & 0x07) <= settings->dmesg_warn_level && continuation != 'c' &&
			    g_regex_match(re, message, 0, NULL)) {
				append_line(&warnings, &warningslen, formatted);
			}
		} else {
			if ((flags & 0x07) <= settings->dmesg_warn_level && continuation != 'c' &&
			    !g_regex_match(re, message, 0, NULL)) {
				append_line(&warnings, &warningslen, formatted);
			}
		}
		append_line(&dmesg, &dmesglen, formatted);
		free(formatted);
	}
	free(line);

	if (current_test != NULL) {
		add_dmesg(current_test, dmesg, dmesglen, warnings, warningslen);
	} else {
		/*
		 * Didn't get any subtest messages at all. If there
		 * are subtests, add all of the dmesg gotten to all of
		 * them.
		 */
		for (i = 0; i < subtests->size; i++) {
			generate_piglit_name(binary, subtests->names[i], piglit_name, sizeof(piglit_name));
			current_test = get_or_create_json_object(tests, piglit_name);
			/*
			 * Don't bother with warnings, any subtests
			 * there are would have skip as their result
			 * anyway.
			 */
			add_dmesg(current_test, dmesg, dmesglen, NULL, 0);
		}

		if (subtests->size == 0) {
			generate_piglit_name(binary, NULL, piglit_name, sizeof(piglit_name));
			current_test = get_or_create_json_object(tests, piglit_name);
			add_dmesg(current_test, dmesg, dmesglen, warnings, warningslen);
		}
	}

	add_empty_dmesgs_where_missing(tests, binary, subtests);

	free(dmesg);
	free(warnings);
	g_regex_unref(re);
	fclose(f);
	return true;
}

static const char *result_from_exitcode(int exitcode)
{
	switch (exitcode) {
	case IGT_EXIT_SKIP:
		return "skip";
	case IGT_EXIT_SUCCESS:
		return "pass";
	case IGT_EXIT_INVALID:
		return "skip";
	case INCOMPLETE_EXITCODE:
		return "incomplete";
	default:
		return "fail";
	}
}

static void add_subtest(struct subtests *subtests, char *subtest)
{
	size_t len = strlen(subtest);
	size_t i;

	if (len == 0)
		return;

	if (subtest[len - 1] == '\n')
		subtest[len - 1] = '\0';

	/* Don't add if we already have this subtest */
	for (i = 0; i < subtests->size; i++)
		if (!strcmp(subtest, subtests->names[i]))
			return;

	subtests->size++;
	subtests->names = realloc(subtests->names, sizeof(*subtests->names) * subtests->size);
	subtests->names[subtests->size - 1] = subtest;
}

static void free_subtests(struct subtests *subtests)
{
	size_t i;

	for (i = 0; i < subtests->size; i++)
		free(subtests->names[i]);
	free(subtests->names);
}

static void fill_from_journal(int fd,
			      struct job_list_entry *entry,
			      struct subtests *subtests,
			      struct results *results)
{
	FILE *f = fdopen(fd, "r");
	char *line = NULL;
	size_t linelen = 0;
	ssize_t read;
	char exitline[] = "exit:";
	char timeoutline[] = "timeout:";
	int exitcode = INCOMPLETE_EXITCODE;
	bool has_timeout = false;
	struct json_object *tests = results->tests;
	struct json_object *runtimes = results->runtimes;

	while ((read = getline(&line, &linelen, f)) > 0) {
		if (read >= strlen(exitline) && !memcmp(line, exitline, strlen(exitline))) {
			char *p = strchr(line, '(');
			char piglit_name[256];
			double time = 0.0;
			struct json_object *obj;

			exitcode = atoi(line + strlen(exitline));

			if (p)
				time = strtod(p + 1, NULL);

			generate_piglit_name(entry->binary, NULL, piglit_name, sizeof(piglit_name));
			obj = get_or_create_json_object(runtimes, piglit_name);
			add_runtime(obj, time);

			/* If no subtests, the test result node also gets the runtime */
			if (subtests->size == 0 && entry->subtest_count == 0) {
				obj = get_or_create_json_object(tests, piglit_name);
				add_runtime(obj, time);
			}
		} else if (read >= strlen(timeoutline) && !memcmp(line, timeoutline, strlen(timeoutline))) {
			has_timeout = true;

			if (subtests->size) {
				/* Assign the timeout to the previously appeared subtest */
				char *last_subtest = subtests->names[subtests->size - 1];
				char piglit_name[256];
				char *p = strchr(line, '(');
				double time = 0.0;
				struct json_object *obj;

				generate_piglit_name(entry->binary, last_subtest, piglit_name, sizeof(piglit_name));
				obj = get_or_create_json_object(tests, piglit_name);

				set_result(obj, "timeout");

				if (p)
					time = strtod(p + 1, NULL);

				/* Add runtime for the subtest... */
				add_runtime(obj, time);

				/* ... and also for the binary */
				generate_piglit_name(entry->binary, NULL, piglit_name, sizeof(piglit_name));
				obj = get_or_create_json_object(runtimes, piglit_name);
				add_runtime(obj, time);
			}
		} else {
			add_subtest(subtests, strdup(line));
		}
	}

	if (subtests->size == 0) {
		char *subtestname = NULL;
		char piglit_name[256];
		struct json_object *obj;
		const char *result = has_timeout ? "timeout" : result_from_exitcode(exitcode);

		/*
		 * If the test was killed before it printed that it's
		 * entering a subtest, we would incorrectly generate
		 * results as the binary had no subtests. If we know
		 * otherwise, do otherwise.
		 */
		if (entry->subtest_count > 0) {
			subtestname = entry->subtests[0];
			add_subtest(subtests, strdup(subtestname));
		}

		generate_piglit_name(entry->binary, subtestname, piglit_name, sizeof(piglit_name));
		obj = get_or_create_json_object(tests, piglit_name);
		set_result(obj, result);
	}

	free(line);
	fclose(f);
}

static void override_result_single(struct json_object *obj)
{
	const char *errtext = "", *result = "";
	struct json_object *textobj;
	bool dmesgwarns = false;

	if (json_object_object_get_ex(obj, "err", &textobj))
		errtext = json_object_get_string(textobj);
	if (json_object_object_get_ex(obj, "result", &textobj))
		result = json_object_get_string(textobj);
	if (json_object_object_get_ex(obj, "dmesg-warnings", &textobj))
		dmesgwarns = true;

	if (!strcmp(result, "pass") &&
	    count_lines(errtext, errtext + strlen(errtext)) > 2) {
		set_result(obj, "warn");
		result = "warn";
	}

	if (dmesgwarns) {
		if (!strcmp(result, "pass") || !strcmp(result, "warn")) {
			set_result(obj, "dmesg-warn");
		} else if (!strcmp(result, "fail")) {
			set_result(obj, "dmesg-fail");
		}
	}
}

static void override_results(char *binary,
			     struct subtests *subtests,
			     struct json_object *tests)
{
	struct json_object *obj;
	char piglit_name[256];
	size_t i;

	if (subtests->size == 0) {
		generate_piglit_name(binary, NULL, piglit_name, sizeof(piglit_name));
		obj = get_or_create_json_object(tests, piglit_name);
		override_result_single(obj);
		return;
	}

	for (i = 0; i < subtests->size; i++) {
		generate_piglit_name(binary, subtests->names[i], piglit_name, sizeof(piglit_name));
		obj = get_or_create_json_object(tests, piglit_name);
		override_result_single(obj);
	}
}

static struct json_object *get_totals_object(struct json_object *totals,
					     const char *key)
{
	struct json_object *obj = NULL;

	if (json_object_object_get_ex(totals, key, &obj))
		return obj;

	obj = json_object_new_object();
	json_object_object_add(totals, key, obj);

	json_object_object_add(obj, "crash", json_object_new_int(0));
	json_object_object_add(obj, "pass", json_object_new_int(0));
	json_object_object_add(obj, "dmesg-fail", json_object_new_int(0));
	json_object_object_add(obj, "dmesg-warn", json_object_new_int(0));
	json_object_object_add(obj, "skip", json_object_new_int(0));
	json_object_object_add(obj, "incomplete", json_object_new_int(0));
	json_object_object_add(obj, "timeout", json_object_new_int(0));
	json_object_object_add(obj, "notrun", json_object_new_int(0));
	json_object_object_add(obj, "fail", json_object_new_int(0));
	json_object_object_add(obj, "warn", json_object_new_int(0));

	return obj;
}

static void add_result_to_totals(struct json_object *totals,
				 const char *result)
{
	json_object *numobj = NULL;
	int old;

	if (!json_object_object_get_ex(totals, result, &numobj)) {
		fprintf(stderr, "Warning: Totals object without count for %s\n", result);
		return;
	}

	old = json_object_get_int(numobj);
	json_object_object_add(totals, result, json_object_new_int(old + 1));
}

static void add_to_totals(const char *binary,
			  struct subtests *subtests,
			  struct results *results)
{
	struct json_object *test, *resultobj, *emptystrtotal, *roottotal, *binarytotal;
	char piglit_name[256];
	const char *result;
	size_t i;

	generate_piglit_name(binary, NULL, piglit_name, sizeof(piglit_name));
	emptystrtotal = get_totals_object(results->totals, "");
	roottotal = get_totals_object(results->totals, "root");
	binarytotal = get_totals_object(results->totals, piglit_name);

	if (subtests->size == 0) {
		test = get_or_create_json_object(results->tests, piglit_name);
		if (!json_object_object_get_ex(test, "result", &resultobj)) {
			fprintf(stderr, "Warning: No results set for %s\n", piglit_name);
			return;
		}
		result = json_object_get_string(resultobj);
		add_result_to_totals(emptystrtotal, result);
		add_result_to_totals(roottotal, result);
		add_result_to_totals(binarytotal, result);
		return;
	}

	for (i = 0; i < subtests->size; i++) {
		generate_piglit_name(binary, subtests->names[i], piglit_name, sizeof(piglit_name));
		test = get_or_create_json_object(results->tests, piglit_name);
		if (!json_object_object_get_ex(test, "result", &resultobj)) {
			fprintf(stderr, "Warning: No results set for %s\n", piglit_name);
			return;
		}
		result = json_object_get_string(resultobj);
		add_result_to_totals(emptystrtotal, result);
		add_result_to_totals(roottotal, result);
		add_result_to_totals(binarytotal, result);
	}
}

static bool parse_test_directory(int dirfd,
				 struct job_list_entry *entry,
				 struct settings *settings,
				 struct results *results)
{
	int fds[_F_LAST];
	struct subtests subtests = {};
	bool status = true;

	if (!open_output_files(dirfd, fds, false)) {
		fprintf(stderr, "Error opening output files\n");
		return false;
	}

	/*
	 * fill_from_journal fills the subtests struct and adds
	 * timeout results where applicable.
	 */
	fill_from_journal(fds[_F_JOURNAL], entry, &subtests, results);

	if (!fill_from_output(fds[_F_OUT], entry->binary, "out", &subtests, results->tests) ||
	    !fill_from_output(fds[_F_ERR], entry->binary, "err", &subtests, results->tests) ||
	    !fill_from_dmesg(fds[_F_DMESG], settings, entry->binary, &subtests, results->tests)) {
		fprintf(stderr, "Error parsing output files\n");
		status = false;
		goto parse_output_end;
	}

	override_results(entry->binary, &subtests, results->tests);
	add_to_totals(entry->binary, &subtests, results);

 parse_output_end:
	close_outputs(fds);
	free_subtests(&subtests);

	return status;
}

static void try_add_notrun_results(const struct job_list_entry *entry,
				   const struct settings *settings,
				   struct results *results)
{
	struct subtests subtests = {};
	struct json_object *current_test;
	size_t i;

	if (entry->subtest_count == 0) {
		char piglit_name[256];

		/* We cannot distinguish no-subtests from run-all-subtests in multiple-mode */
		if (settings->multiple_mode)
			return;
		generate_piglit_name(entry->binary, NULL, piglit_name, sizeof(piglit_name));
		current_test = get_or_create_json_object(results->tests, piglit_name);
		json_object_object_add(current_test, "out", json_object_new_string(""));
		json_object_object_add(current_test, "err", json_object_new_string(""));
		json_object_object_add(current_test, "dmesg", json_object_new_string(""));
		json_object_object_add(current_test, "result", json_object_new_string("notrun"));
	}

	for (i = 0; i < entry->subtest_count; i++) {
		char piglit_name[256];

		generate_piglit_name(entry->binary, entry->subtests[i], piglit_name, sizeof(piglit_name));
		current_test = get_or_create_json_object(results->tests, piglit_name);
		json_object_object_add(current_test, "out", json_object_new_string(""));
		json_object_object_add(current_test, "err", json_object_new_string(""));
		json_object_object_add(current_test, "dmesg", json_object_new_string(""));
		json_object_object_add(current_test, "result", json_object_new_string("notrun"));
		add_subtest(&subtests, strdup(entry->subtests[i]));
	}

	add_to_totals(entry->binary, &subtests, results);
	free_subtests(&subtests);
}

static void create_result_root_nodes(struct json_object *root,
				     struct results *results)
{
	results->tests = json_object_new_object();
	json_object_object_add(root, "tests", results->tests);
	results->totals = json_object_new_object();
	json_object_object_add(root, "totals", results->totals);
	results->runtimes = json_object_new_object();
	json_object_object_add(root, "runtimes", results->runtimes);
}

struct json_object *generate_results_json(int dirfd)
{
	struct settings settings;
	struct job_list job_list;
	struct json_object *obj, *elapsed;
	struct results results;
	int testdirfd, fd;
	size_t i;

	init_settings(&settings);
	init_job_list(&job_list);

	if (!read_settings_from_dir(&settings, dirfd)) {
		fprintf(stderr, "resultgen: Cannot parse settings\n");
		return NULL;
	}

	if (!read_job_list(&job_list, dirfd)) {
		fprintf(stderr, "resultgen: Cannot parse job list\n");
		return NULL;
	}

	obj = json_object_new_object();
	json_object_object_add(obj, "__type__", json_object_new_string("TestrunResult"));
	json_object_object_add(obj, "results_version", json_object_new_int(10));
	json_object_object_add(obj, "name",
			       settings.name ?
			       json_object_new_string(settings.name) :
			       json_object_new_string(""));

	if ((fd = openat(dirfd, "uname.txt", O_RDONLY)) >= 0) {
		char buf[128];
		ssize_t r = read(fd, buf, sizeof(buf));

		if (r > 0 && buf[r - 1] == '\n')
			r--;

		json_object_object_add(obj, "uname",
				       json_object_new_string_len(buf, r));
		close(fd);
	}

	elapsed = json_object_new_object();
	json_object_object_add(elapsed, "__type__", json_object_new_string("TimeAttribute"));
	if ((fd = openat(dirfd, "starttime.txt", O_RDONLY)) >= 0) {
		char buf[128] = {};
		read(fd, buf, sizeof(buf));
		json_object_object_add(elapsed, "start", json_object_new_double(atof(buf)));
		close(fd);
	}
	if ((fd = openat(dirfd, "endtime.txt", O_RDONLY)) >= 0) {
		char buf[128] = {};
		read(fd, buf, sizeof(buf));
		json_object_object_add(elapsed, "end", json_object_new_double(atof(buf)));
		close(fd);
	}
	json_object_object_add(obj, "time_elapsed", elapsed);

	create_result_root_nodes(obj, &results);

	/*
	 * Result fields that won't be added:
	 *
	 * - glxinfo
	 * - wglinfo
	 * - clinfo
	 *
	 * Result fields that are TODO:
	 *
	 * - lspci
	 * - options
	 */

	for (i = 0; i < job_list.size; i++) {
		char name[16];

		snprintf(name, 16, "%zd", i);
		if ((testdirfd = openat(dirfd, name, O_DIRECTORY | O_RDONLY)) < 0) {
			try_add_notrun_results(&job_list.entries[i], &settings, &results);
			continue;
		}

		if (!parse_test_directory(testdirfd, &job_list.entries[i], &settings, &results)) {
			close(testdirfd);
			return NULL;
		}
		close(testdirfd);
	}

	if ((fd = openat(dirfd, "aborted.txt", O_RDONLY)) >= 0) {
		char buf[4096];
		char piglit_name[] = "igt@runner@aborted";
		struct subtests abortsub = {};
		struct json_object *aborttest = get_or_create_json_object(results.tests, piglit_name);
		ssize_t s;

		add_subtest(&abortsub, strdup("aborted"));

		s = read(fd, buf, sizeof(buf));

		json_object_object_add(aborttest, "out",
				       json_object_new_string_len(buf, s));
		json_object_object_add(aborttest, "err",
				       json_object_new_string(""));
		json_object_object_add(aborttest, "dmesg",
				       json_object_new_string(""));
		json_object_object_add(aborttest, "result",
				       json_object_new_string("fail"));

		add_to_totals("runner", &abortsub, &results);

		free_subtests(&abortsub);
	}

	free_settings(&settings);
	free_job_list(&job_list);

	return obj;
}

bool generate_results(int dirfd)
{
	struct json_object *obj = generate_results_json(dirfd);
	const char *json_string;
	int resultsfd;

	if (obj == NULL)
		return false;

	/* TODO: settings.overwrite */
	if ((resultsfd = openat(dirfd, "results.json", O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) {
		fprintf(stderr, "resultgen: Cannot create results file\n");
		return false;
	}

	json_string = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PRETTY);
	write(resultsfd, json_string, strlen(json_string));
	close(resultsfd);
	return true;
}

bool generate_results_path(char *resultspath)
{
	int dirfd = open(resultspath, O_DIRECTORY | O_RDONLY);

	if (dirfd < 0)
		return false;

	return generate_results(dirfd);
}
