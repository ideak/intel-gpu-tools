#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <json.h>

#include "igt_aux.h"
#include "igt_core.h"
#include "runnercomms.h"
#include "resultgen.h"
#include "settings.h"
#include "executor.h"
#include "output_strings.h"

#define INCOMPLETE_EXITCODE -1234
#define GRACEFUL_EXITCODE -SIGHUP

_Static_assert(INCOMPLETE_EXITCODE != IGT_EXIT_SKIP, "exit code clash");
_Static_assert(INCOMPLETE_EXITCODE != IGT_EXIT_SUCCESS, "exit code clash");
_Static_assert(INCOMPLETE_EXITCODE != IGT_EXIT_INVALID, "exit code clash");
_Static_assert(INCOMPLETE_EXITCODE != GRACEFUL_EXITCODE, "exit code clash");

struct subtest
{
	char *name;
	char **dynamic_names;
	size_t dynamic_size;
};

struct subtest_list
{
	struct subtest *subs;
	size_t size;
};

struct results
{
	struct json_object *tests;
	struct json_object *totals;
	struct json_object *runtimes;
};

static void add_dynamic_subtest(struct subtest *subtest, char *dynamic)
{
	size_t len = strlen(dynamic);
	size_t i;

	if (len == 0)
		return;

	if (dynamic[len - 1] == '\n')
		dynamic[len - 1] = '\0';

	/* Don't add if we already have this one */
	for (i = 0; i < subtest->dynamic_size; i++)
		if (!strcmp(dynamic, subtest->dynamic_names[i]))
			return;

	subtest->dynamic_size++;
	subtest->dynamic_names = realloc(subtest->dynamic_names, sizeof(*subtest->dynamic_names) * subtest->dynamic_size);
	subtest->dynamic_names[subtest->dynamic_size - 1] = dynamic;
}

static void add_subtest(struct subtest_list *subtests, char *subtest)
{
	size_t len = strlen(subtest);
	size_t i;

	if (len == 0)
		return;

	if (subtest[len - 1] == '\n')
		subtest[len - 1] = '\0';

	/* Don't add if we already have this subtest */
	for (i = 0; i < subtests->size; i++)
		if (!strcmp(subtest, subtests->subs[i].name))
			return;

	subtests->size++;
	subtests->subs = realloc(subtests->subs, sizeof(*subtests->subs) * subtests->size);
	memset(&subtests->subs[subtests->size - 1], 0, sizeof(struct subtest));
	subtests->subs[subtests->size - 1].name = subtest;
}

static void free_subtest(struct subtest *subtest)
{
	size_t i;

	for (i = 0; i < subtest->dynamic_size; i++)
		free(subtest->dynamic_names[i]);
	free(subtest->dynamic_names);
}

static void free_subtests(struct subtest_list *subtests)
{
	size_t i;

	for (i = 0; i < subtests->size; i++)
		free_subtest(&subtests->subs[i]);
	free(subtests->subs);
}

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

static void append_line(char **buf, size_t *buflen, const char *line)
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
				 const char *resulttextprefix,
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
	 *
	 * For dynamic subtests the same structure applies, but the
	 * string "Subtest " is "Dynamic subtest "
	 * instead. (`DYNAMIC_SUBTEST_RESULT` from output_strings.h)
	 */

	if (!line)
		return;

	line_end = memchr(line, '\n', bufend - line);
	linelen = line_end != NULL ? line_end - line : bufend - line;

	if (strlen(resulttextprefix) + subtestlen + strlen(": ") > linelen ||
	    strncmp(line + strlen(resulttextprefix), subtest, subtestlen)) {
		/* This is not the correct result line */
		return;
	}

	resultstring = line + strlen(resulttextprefix) + subtestlen + strlen(": ");
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

struct match_needle
{
	const char *str;
	bool (*validate)(const char *needle, const char *line, const char *bufend);
};

static void match_add(struct matches *matches, const char *where, const char *what)
{
	struct match_item newitem = { where, what };

	matches->size++;
	matches->items = realloc(matches->items, matches->size * sizeof(*matches->items));
	matches->items[matches->size - 1] = newitem;
}

static struct matches find_matches(const char *buf, const char *bufend,
				   const struct match_needle *needles)
{
	struct matches ret = {};

	while (buf < bufend) {
		const struct match_needle *needle;

		for (needle = needles; needle->str; needle++) {
			if (bufend - buf < strlen(needle->str))
				continue;

			if (!memcmp(buf, needle->str, strlen(needle->str)) &&
			    (!needle->validate || needle->validate(needle->str, buf, bufend))) {
				match_add(&ret, buf, needle->str);
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

static bool valid_char_for_subtest_name(char x)
{
	return x == '-' || x == '_' || isalnum(x);
}

static bool is_subtest_result_line(const char *needle, const char *line, const char *bufend)
{
	line += strlen(needle);

	/*
	 * At this point of the string we're expecting:
	 * - The subtest name (one or more of a-z, A-Z, 0-9, '-' and '_')
	 * - The characters ':' and ' '
	 *
	 * If we find all those, allow parsing this line as [dynamic]
	 * subtest result.
	 */

	if (!valid_char_for_subtest_name(*line))
		return false;

	while (line < bufend && valid_char_for_subtest_name(*line))
		line++;

	if (line >= bufend || *line++ != ':')
		return false;

	if (line >= bufend || *line++ != ' ')
		return false;

	return true;
}

static void free_matches(struct matches *matches)
{
	free(matches->items);
}

static struct json_object *new_escaped_json_string(const char *buf, size_t len)
{
	struct json_object *obj;
	char *str = NULL;
	size_t strsize = 0;
	size_t i;

	/*
	 * Test output may be garbage; strings passed to json-c need to be
	 * UTF-8 encoded so any non-ASCII characters are converted to their
	 * UTF-8 representation, which requires 2 bytes per character.
	 */
	str = malloc(len * 2);
	if (!str)
		return NULL;

	for (i = 0; i < len; i++) {
		if (buf[i] > 0 && buf[i] < 128) {
			str[strsize] = buf[i];
			++strsize;
		} else {
			/* Encode > 128 character to UTF-8. */
			str[strsize] = ((unsigned char)buf[i] >> 6) | 0xC0;
			str[strsize + 1] = ((unsigned char)buf[i] & 0x3F) | 0x80;
			strsize += 2;
		}
	}

	obj = json_object_new_string_len(str, strsize);
	free(str);

	return obj;
}

static void add_igt_version(struct json_object *testobj,
			    const char *igt_version,
			    size_t igt_version_len)
{
	if (igt_version)
		json_object_object_add(testobj, "igt-version",
				       new_escaped_json_string(igt_version, igt_version_len));
}

enum subtest_find_pattern {
	PATTERN_BEGIN,
	PATTERN_RESULT,
};

static int find_subtest_idx_limited(struct matches matches,
				    const char *bufend,
				    const char *linekey,
				    enum subtest_find_pattern pattern,
				    const char *subtest_name,
				    int first,
				    int last)
{
	char *full_line;
	int line_len;
	int k;

	switch (pattern) {
	case PATTERN_BEGIN:
		line_len = asprintf(&full_line, "%s%s\n", linekey, subtest_name);
		break;
	case PATTERN_RESULT:
		line_len = asprintf(&full_line, "%s%s: ", linekey, subtest_name);
		break;
	default:
		assert(!"Unknown pattern");
	}

	if (line_len < 0)
		return -1;

	for (k = first; k < last; k++) {
		ptrdiff_t rem = bufend - matches.items[k].where;

		if (matches.items[k].what == linekey &&
		    !memcmp(matches.items[k].where,
			    full_line,
			    min_t(ptrdiff_t, line_len, rem)))
			break;
	}

	free(full_line);

	if (k == last)
		k = -1;

	return k;
}

static int find_subtest_idx(struct matches matches,
			    const char *bufend,
			    const char *linekey,
			    enum subtest_find_pattern pattern,
			    const char *subtest_name)
{
	return find_subtest_idx_limited(matches, bufend, linekey, pattern, subtest_name, 0, matches.size);
}

static const char *find_subtest_begin_limit_limited(struct matches matches,
						    int begin_idx,
						    int result_idx,
						    const char *buf,
						    const char *bufend,
						    int first_idx)
{
	/* No matching output at all, include everything */
	if (begin_idx < first_idx && result_idx < first_idx)
		return buf;

	if (begin_idx < first_idx) {
		/*
		 * Subtest didn't start, but we have the
		 * result. Probably because an igt_fixture
		 * made it fail/skip.
		 *
		 * We go backwards one match from the result match,
		 * and start from the next line.
		 */
		if (result_idx > first_idx)
			return next_line(matches.items[result_idx - 1].where, bufend);
		else
			return buf;
	}

	/* Include all non-special output before the beginning line. */
	if (begin_idx <= first_idx)
		return buf;

	return next_line(matches.items[begin_idx - 1].where, bufend);
}

static const char *find_subtest_begin_limit(struct matches matches,
					    int begin_idx,
					    int result_idx,
					    const char *buf,
					    const char *bufend)
{
	return find_subtest_begin_limit_limited(matches, begin_idx, result_idx, buf, bufend, 0);
}

static const char *find_subtest_end_limit_limited(struct matches matches,
						  int begin_idx,
						  int result_idx,
						  const char *buf,
						  const char *bufend,
						  int first_idx,
						  int last_idx)
{
	int k;

	/* No matching output at all, include everything */
	if (begin_idx < first_idx && result_idx < first_idx)
		return bufend;

	if (result_idx < first_idx) {
		/*
		 * Incomplete result. Include all output up to the
		 * next starting subtest, or the result of one.
		 */
		for (k = begin_idx + 1; k < last_idx; k++) {
			if (matches.items[k].what == STARTING_SUBTEST ||
			    matches.items[k].what == SUBTEST_RESULT)
				return matches.items[k].where;
		}

		return bufend;
	}

	/* Include all non-special output to the next match, whatever it is. */
	if (result_idx < last_idx - 1)
		return matches.items[result_idx + 1].where;

	return bufend;
}

static const char *find_subtest_end_limit(struct matches matches,
					  int begin_idx,
					  int result_idx,
					  const char *buf,
					  const char *bufend)
{
	return find_subtest_end_limit_limited(matches, begin_idx, result_idx, buf, bufend, 0, matches.size);
}

static void process_dynamic_subtest_output(const char *piglit_name,
					   const char *igt_version,
					   size_t igt_version_len,
					   struct matches matches,
					   int begin_idx,
					   int result_idx,
					   const char *beg,
					   const char *end,
					   const char *key,
					   struct json_object *tests,
					   struct subtest *subtest)
{
	size_t k;

	if (result_idx < 0) {
		/* If the subtest itself is incomplete, stop at the next start/end of a subtest */
		for (result_idx = begin_idx + 1;
		     result_idx < matches.size;
		     result_idx++) {
			if (matches.items[result_idx].what == STARTING_SUBTEST ||
			    matches.items[result_idx].what == SUBTEST_RESULT)
				break;
		}
	}

	for (k = begin_idx + 1; k < result_idx; k++) {
		struct json_object *current_dynamic_test = NULL;
		int dyn_result_idx;
		char dynamic_name[256];
		char dynamic_piglit_name[256];
		const char *dynbeg, *dynend;

		if (matches.items[k].what != STARTING_DYNAMIC_SUBTEST)
			continue;

		if (sscanf(matches.items[k].where + strlen(STARTING_DYNAMIC_SUBTEST), "%s", dynamic_name) != 1) {
			/* Cannot parse name, just ignore this one */
			continue;
		}

		dyn_result_idx = find_subtest_idx_limited(matches, end, DYNAMIC_SUBTEST_RESULT, PATTERN_RESULT, dynamic_name, k, result_idx);

		dynbeg = find_subtest_begin_limit_limited(matches, k, dyn_result_idx, beg, end, begin_idx + 1);
		dynend = find_subtest_end_limit_limited(matches, k, dyn_result_idx, beg, end, begin_idx + 1, result_idx);

		generate_piglit_name_for_dynamic(piglit_name, dynamic_name, dynamic_piglit_name, sizeof(dynamic_piglit_name));

		add_dynamic_subtest(subtest, strdup(dynamic_name));
		current_dynamic_test = get_or_create_json_object(tests, dynamic_piglit_name);

		json_object_object_add(current_dynamic_test, key,
				       new_escaped_json_string(dynbeg, dynend - dynbeg));
		add_igt_version(current_dynamic_test, igt_version, igt_version_len);

		if (!json_object_object_get_ex(current_dynamic_test, "result", NULL)) {
			const char *dynresulttext;
			double dyntime;

			parse_subtest_result(dynamic_name,
					     DYNAMIC_SUBTEST_RESULT,
					     &dynresulttext, &dyntime,
					     dyn_result_idx < 0 ? NULL : matches.items[dyn_result_idx].where,
					     dynend);

			/*
			 * If a dynamic subsubtest is considered
			 * incomplete we need to check parent's status
			 * first, to be sure that the binary hasn't
			 * aborted or stopped gracefully (exit
			 * code). If it has aborted then we have to
			 * attribute this status to our subsubtest.
			 */
			if (!strcmp(dynresulttext, "incomplete")) {
				struct json_object *parent_subtest;

				if (json_object_object_get_ex(tests, piglit_name, &parent_subtest) &&
				    json_object_object_get_ex(parent_subtest, "result", &parent_subtest)) {
					const char *resulttext = json_object_get_string(parent_subtest);

					if (!strcmp(resulttext, "abort") ||
					    !strcmp(resulttext, "notrun"))
						dynresulttext = resulttext;
				}
			}

			set_result(current_dynamic_test, dynresulttext);
			set_runtime(current_dynamic_test, dyntime);
		}
	}
}

static bool fill_from_output(int fd, const char *binary, const char *key,
			     struct subtest_list *subtests,
			     struct json_object *tests)
{
	char *buf, *bufend, *nullchr;
	struct stat statbuf;
	char piglit_name[256];
	char *igt_version = NULL;
	size_t igt_version_len = 0;
	struct json_object *current_test = NULL;
	struct match_needle needles[] = {
		{ STARTING_SUBTEST, NULL },
		{ SUBTEST_RESULT, is_subtest_result_line },
		{ STARTING_DYNAMIC_SUBTEST, NULL },
		{ DYNAMIC_SUBTEST_RESULT, is_subtest_result_line },
		{ NULL, NULL },
	};
	struct matches matches = {};
	size_t i;

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
				       new_escaped_json_string(buf, statbuf.st_size));
		add_igt_version(current_test, igt_version, igt_version_len);

		return true;
	}

	matches = find_matches(buf, bufend, needles);

	for (i = 0; i < subtests->size; i++) {
		int begin_idx, result_idx;
		const char *resulttext;
		const char *beg, *end;
		double time;

		generate_piglit_name(binary, subtests->subs[i].name, piglit_name, sizeof(piglit_name));
		current_test = get_or_create_json_object(tests, piglit_name);

		begin_idx = find_subtest_idx(matches, bufend, STARTING_SUBTEST, PATTERN_BEGIN, subtests->subs[i].name);
		result_idx = find_subtest_idx(matches, bufend, SUBTEST_RESULT, PATTERN_RESULT, subtests->subs[i].name);

		beg = find_subtest_begin_limit(matches, begin_idx, result_idx, buf, bufend);
		end = find_subtest_end_limit(matches, begin_idx, result_idx, buf, bufend);

		json_object_object_add(current_test, key,
				       new_escaped_json_string(beg, end - beg));

		add_igt_version(current_test, igt_version, igt_version_len);

		if (!json_object_object_get_ex(current_test, "result", NULL)) {
			parse_subtest_result(subtests->subs[i].name,
					     SUBTEST_RESULT,
					     &resulttext, &time,
					     result_idx < 0 ? NULL : matches.items[result_idx].where,
					     end);
			set_result(current_test, resulttext);
			set_runtime(current_test, time);
		}

		process_dynamic_subtest_output(piglit_name,
					       igt_version, igt_version_len,
					       matches,
					       begin_idx, result_idx,
					       beg, end,
					       key,
					       tests,
					       &subtests->subs[i]);
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
	"i915: probe of [0-9a-fA-F:.]+ failed with error -25" _
	/* swiotbl warns even when asked not to */
	"mock: DMA: Out of SW-IOMMU space for [0-9]+ bytes" _
	"usb usb[0-9]+: root hub lost power or was reset"
	;
#undef _

static const char igt_piglit_style_dmesg_blacklist[] =
	"(\\[drm:|drm_|intel_|i915_|\\[drm\\])";

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
	if (*message == NULL) {
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
			       new_escaped_json_string(dmesg, dmesglen));

	if (warnings) {
		json_object_object_add(obj, "dmesg-warnings",
				       new_escaped_json_string(warnings, warningslen));
	}
}

static void add_empty_dmesgs_where_missing(struct json_object *tests,
					   char *binary,
					   struct subtest_list *subtests)
{
	struct json_object *current_test;
	char piglit_name[256];
	char dynamic_piglit_name[256];
	size_t i, k;

	for (i = 0; i < subtests->size; i++) {
		generate_piglit_name(binary, subtests->subs[i].name, piglit_name, sizeof(piglit_name));
		current_test = get_or_create_json_object(tests, piglit_name);
		if (!json_object_object_get_ex(current_test, "dmesg", NULL)) {
			add_dmesg(current_test, "", 0, NULL, 0);
		}

		for (k = 0; k < subtests->subs[i].dynamic_size; k++) {
			generate_piglit_name_for_dynamic(piglit_name, subtests->subs[i].dynamic_names[k],
							 dynamic_piglit_name, sizeof(dynamic_piglit_name));
			current_test = get_or_create_json_object(tests, dynamic_piglit_name);
			if (!json_object_object_get_ex(current_test, "dmesg", NULL)) {
				add_dmesg(current_test, "", 0, NULL, 0);
			}
		}
	}

}

static bool fill_from_dmesg(int fd,
			    struct settings *settings,
			    char *binary,
			    struct subtest_list *subtests,
			    struct json_object *tests)
{
	char *line = NULL;
	char *warnings = NULL, *dynamic_warnings = NULL;
	char *dmesg = NULL, *dynamic_dmesg = NULL;
	size_t linelen = 0;
	size_t warningslen = 0, dynamic_warnings_len = 0;
	size_t dmesglen = 0, dynamic_dmesg_len = 0;
	struct json_object *current_test = NULL;
	struct json_object *current_dynamic_test = NULL;
	FILE *f = fdopen(fd, "r");
	char piglit_name[256];
	char dynamic_piglit_name[256];
	size_t i;
	GRegex *re;

	if (!f) {
		return false;
	}

	if (!init_regex_whitelist(settings, &re)) {
		fclose(f);
		return false;
	}

	while (getline(&line, &linelen, f) > 0) {
		char *formatted;
		unsigned flags;
		unsigned long long ts_usec;
		char continuation;
		char *message, *subtest, *dynamic_subtest;

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

				if (current_dynamic_test != NULL)
					add_dmesg(current_dynamic_test, dynamic_dmesg, dynamic_dmesg_len, dynamic_warnings, dynamic_warnings_len);

				free(dynamic_dmesg);
				free(dynamic_warnings);
				dynamic_dmesg = dynamic_warnings = NULL;
				dynamic_dmesg_len = dynamic_warnings_len = 0;
				current_dynamic_test = NULL;
			}

			subtest += strlen(STARTING_SUBTEST_DMESG);
			generate_piglit_name(binary, subtest, piglit_name, sizeof(piglit_name));
			current_test = get_or_create_json_object(tests, piglit_name);
		}

		if (current_test != NULL &&
		    (dynamic_subtest = strstr(message, STARTING_DYNAMIC_SUBTEST_DMESG)) != NULL) {
			if (current_dynamic_test != NULL) {
				/* Done with the previous dynamic subtest, file up */
				add_dmesg(current_dynamic_test, dynamic_dmesg, dynamic_dmesg_len, dynamic_warnings, dynamic_warnings_len);

				free(dynamic_dmesg);
				free(dynamic_warnings);
				dynamic_dmesg = dynamic_warnings = NULL;
				dynamic_dmesg_len = dynamic_warnings_len = 0;
			}

			dynamic_subtest += strlen(STARTING_DYNAMIC_SUBTEST_DMESG);
			generate_piglit_name_for_dynamic(piglit_name, dynamic_subtest, dynamic_piglit_name, sizeof(dynamic_piglit_name));
			current_dynamic_test = get_or_create_json_object(tests, dynamic_piglit_name);
		}

		if (settings->piglit_style_dmesg) {
			if ((flags & 0x07) <= settings->dmesg_warn_level && continuation != 'c' &&
			    g_regex_match(re, message, 0, NULL)) {
				append_line(&warnings, &warningslen, formatted);
				if (current_test != NULL)
					append_line(&dynamic_warnings, &dynamic_warnings_len, formatted);
			}
		} else {
			if ((flags & 0x07) <= settings->dmesg_warn_level && continuation != 'c' &&
			    !g_regex_match(re, message, 0, NULL)) {
				append_line(&warnings, &warningslen, formatted);
				if (current_test != NULL)
					append_line(&dynamic_warnings, &dynamic_warnings_len, formatted);
			}
		}
		append_line(&dmesg, &dmesglen, formatted);
		append_line(&dynamic_dmesg, &dynamic_dmesg_len, formatted);
		free(formatted);
	}
	free(line);

	if (current_test != NULL) {
		add_dmesg(current_test, dmesg, dmesglen, warnings, warningslen);
		if (current_dynamic_test != NULL) {
			add_dmesg(current_dynamic_test, dynamic_dmesg, dynamic_dmesg_len, dynamic_warnings, dynamic_warnings_len);
		}
	} else {
		/*
		 * Didn't get any subtest messages at all. If there
		 * are subtests, add all of the dmesg gotten to all of
		 * them.
		 */
		for (i = 0; i < subtests->size; i++) {
			generate_piglit_name(binary, subtests->subs[i].name, piglit_name, sizeof(piglit_name));
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
	free(dynamic_dmesg);
	free(warnings);
	free(dynamic_warnings);
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
	case IGT_EXIT_ABORT:
		return "abort";
	case INCOMPLETE_EXITCODE:
		return "incomplete";
	case GRACEFUL_EXITCODE:
		return "notrun";
	default:
		return "fail";
	}
}

static void fill_from_journal(int fd,
			      struct job_list_entry *entry,
			      struct subtest_list *subtests,
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
				char *last_subtest = subtests->subs[subtests->size - 1].name;
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

	if (subtests->size && (exitcode == IGT_EXIT_ABORT || exitcode == GRACEFUL_EXITCODE)) {
		char *last_subtest = subtests->subs[subtests->size - 1].name;
		char subtest_piglit_name[256];
		struct json_object *subtest_obj;

		generate_piglit_name(entry->binary, last_subtest, subtest_piglit_name, sizeof(subtest_piglit_name));
		subtest_obj = get_or_create_json_object(tests, subtest_piglit_name);

		set_result(subtest_obj, exitcode == IGT_EXIT_ABORT ? "abort" : "notrun");
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

typedef enum comms_state {
	STATE_INITIAL = 0,
	STATE_AFTER_EXEC,
	STATE_SUBTEST_STARTED,
	STATE_DYNAMIC_SUBTEST_STARTED,
	STATE_BETWEEN_DYNAMIC_SUBTESTS,
	STATE_BETWEEN_SUBTESTS,
	STATE_EXITED,
} comms_state_t;

struct comms_context
{
	comms_state_t state;

	struct json_object *binaryruntimeobj;
	struct json_object *current_test;
	struct json_object *current_dynamic_subtest;
	char *current_subtest_name;
	char *current_dynamic_subtest_name;

	char *outbuf, *errbuf;
	size_t outbuflen, errbuflen;
	size_t outidx, nextoutidx;
	size_t erridx, nexterridx;
	size_t dynoutidx, nextdynoutidx;
	size_t dynerridx, nextdynerridx;

	char *igt_version;

	char *subtestresult;
	char *dynamicsubtestresult;

	char *cmdline;
	int exitcode;

	struct subtest_list *subtests;
	struct subtest *subtest;
	struct results *results;
	struct job_list_entry *entry;
	const char *binary;
};

static void comms_free_context(struct comms_context *context)
{
	free(context->current_subtest_name);
	free(context->current_dynamic_subtest_name);
	free(context->outbuf);
	free(context->errbuf);
	free(context->igt_version);
	free(context->subtestresult);
	free(context->dynamicsubtestresult);
	free(context->cmdline);
}

static void comms_inject_subtest_start_log(struct comms_context *context,
					   const char *prefix,
					   const char *subtestname)
{
	char msg[512];

	snprintf(msg, sizeof(msg), "%s%s\n", prefix, subtestname);
	append_line(&context->outbuf, &context->outbuflen, msg);
	append_line(&context->errbuf, &context->errbuflen, msg);
}

static void comms_inject_subtest_end_log(struct comms_context *context,
					 const char *prefix,
					 const char *subtestname,
					 const char *subtestresult,
					 const char *timeused)
{
	char msg[512];

	snprintf(msg, sizeof(msg), "%s%s: %s (%ss)\n", prefix, subtestname, subtestresult, timeused);
	append_line(&context->outbuf, &context->outbuflen, msg);
	append_line(&context->errbuf, &context->errbuflen, msg);
}

static void comms_finish_subtest(struct comms_context *context)
{
	json_object_object_add(context->current_test, "out",
			       new_escaped_json_string(context->outbuf + context->outidx, context->outbuflen - context->outidx));
	json_object_object_add(context->current_test, "err",
			       new_escaped_json_string(context->errbuf + context->outidx, context->errbuflen - context->erridx));

	if (context->igt_version)
		add_igt_version(context->current_test, context->igt_version, strlen(context->igt_version));

	if (context->subtestresult == NULL)
		context->subtestresult = strdup("incomplete");
	set_result(context->current_test, context->subtestresult);

	free(context->subtestresult);
	context->subtestresult = NULL;
	context->current_test = NULL;

	context->outidx = context->nextoutidx;
	context->erridx = context->nexterridx;
}

static void comms_finish_dynamic_subtest(struct comms_context *context)
{
	json_object_object_add(context->current_dynamic_subtest, "out",
			       new_escaped_json_string(context->outbuf + context->dynoutidx, context->outbuflen - context->dynoutidx));
	json_object_object_add(context->current_dynamic_subtest, "err",
			       new_escaped_json_string(context->errbuf + context->dynerridx, context->errbuflen - context->dynerridx));

	if (context->igt_version)
		add_igt_version(context->current_dynamic_subtest, context->igt_version, strlen(context->igt_version));

	if (context->dynamicsubtestresult == NULL)
		context->dynamicsubtestresult = strdup("incomplete");
	set_result(context->current_dynamic_subtest, context->dynamicsubtestresult);

	free(context->dynamicsubtestresult);
	context->dynamicsubtestresult = NULL;
	context->current_dynamic_subtest = NULL;

	context->dynoutidx = context->nextdynoutidx;
	context->dynerridx = context->nextdynerridx;
}

static void comms_add_new_subtest(struct comms_context *context,
				  const char *subtestname)
{
	char piglit_name[256];

	add_subtest(context->subtests, strdup(subtestname));
	context->subtest = &context->subtests->subs[context->subtests->size - 1];
	generate_piglit_name(context->binary, subtestname, piglit_name, sizeof(piglit_name));
	context->current_test = get_or_create_json_object(context->results->tests, piglit_name);
	free(context->current_subtest_name);
	context->current_subtest_name = strdup(subtestname);
}

static void comms_add_new_dynamic_subtest(struct comms_context *context,
					  const char *dynamic_name)
{
	char piglit_name[256];
	char dynamic_piglit_name[256];

	add_dynamic_subtest(context->subtest, strdup(dynamic_name));
	generate_piglit_name(context->binary, context->current_subtest_name, piglit_name, sizeof(piglit_name));
	generate_piglit_name_for_dynamic(piglit_name, dynamic_name, dynamic_piglit_name, sizeof(dynamic_piglit_name));
	context->current_dynamic_subtest = get_or_create_json_object(context->results->tests, dynamic_piglit_name);
	free(context->current_dynamic_subtest_name);
	context->current_dynamic_subtest_name = strdup(dynamic_name);
}

static bool comms_handle_log(const struct runnerpacket *packet,
			     runnerpacket_read_helper helper,
			     void *userdata)
{
	struct comms_context *context = userdata;
	char **textbuf;
	size_t *textlen;

	if (helper.log.stream == STDOUT_FILENO) {
		textbuf = &context->outbuf;
		textlen = &context->outbuflen;
	} else {
		textbuf = &context->errbuf;
		textlen = &context->errbuflen;
	}
	append_line(textbuf, textlen, helper.log.text);

	return true;
}

static bool comms_handle_exec(const struct runnerpacket *packet,
			      runnerpacket_read_helper helper,
			      void *userdata)
{
	struct comms_context *context = userdata;

	switch (context->state) {
	case STATE_INITIAL:
		break;

	case STATE_AFTER_EXEC:
		/*
		 * Resume after an exec that didn't involve any
		 * subtests. Resumes can only happen for tests with
		 * subtests, so while we might have logs already
		 * collected, we have nowhere to put them. The joblist
		 * doesn't help, because the ordering is up to the
		 * test.
		 */
		printf("Warning: Need to discard %zd bytes of logs, no subtest data\n", context->outbuflen + context->errbuflen);
		context->outbuflen = context->errbuflen = 0;
		context->outidx = context->erridx = 0;
		context->nextoutidx = context->nexterridx = 0;
		break;

	case STATE_SUBTEST_STARTED:
	case STATE_DYNAMIC_SUBTEST_STARTED:
	case STATE_BETWEEN_DYNAMIC_SUBTESTS:
	case STATE_BETWEEN_SUBTESTS:
	case STATE_EXITED:
		/* A resume exec, so we're already collecting data. */
		assert(context->current_test != NULL);
		comms_finish_subtest(context);
		break;
	default:
		assert(false); /* unreachable */
	}

	free(context->cmdline);
	context->cmdline = strdup(helper.exec.cmdline);

	context->state = STATE_AFTER_EXEC;

	return true;
}

static bool comms_handle_exit(const struct runnerpacket *packet,
			      runnerpacket_read_helper helper,
			      void *userdata)
{
	struct comms_context *context = userdata;
	char piglit_name[256];

	if (context->state == STATE_AFTER_EXEC) {
		/*
		 * Exit after exec, so we didn't get any
		 * subtests. Check if there's supposed to be any,
		 * otherwise stuff logs into the binary's result.
		 */

		char *subtestname = NULL;

		if (context->entry->subtest_count > 0) {
			subtestname = context->entry->subtests[0];
			add_subtest(context->subtests, strdup(subtestname));
		}
		generate_piglit_name(context->binary, subtestname, piglit_name, sizeof(piglit_name));
		context->current_test = get_or_create_json_object(context->results->tests, piglit_name);

		/* Get result from exitcode unless we have an override already */
		if (context->subtestresult == NULL)
			context->subtestresult = strdup(result_from_exitcode(helper.exit.exitcode));
	} else if (helper.exit.exitcode == IGT_EXIT_ABORT || helper.exit.exitcode == GRACEFUL_EXITCODE) {
		/*
		 * If we did get subtests, we need to assign the
		 * special exitcode results to the last subtest,
		 * normal and dynamic
		 */
		const char *result = helper.exit.exitcode == IGT_EXIT_ABORT ? "abort" : "notrun";

		free(context->subtestresult);
		context->subtestresult = strdup(result);
		free(context->dynamicsubtestresult);
		context->dynamicsubtestresult = strdup(result);
	}

	context->exitcode = helper.exit.exitcode;
	add_runtime(context->binaryruntimeobj, strtod(helper.exit.timeused, NULL));

	context->state = STATE_EXITED;

	return true;
}

static bool comms_handle_subtest_start(const struct runnerpacket *packet,
				       runnerpacket_read_helper helper,
				       void *userdata)
{
	struct comms_context *context = userdata;
	char errmsg[512];

	switch (context->state) {
	case STATE_INITIAL:
	case STATE_EXITED:
		/* Subtest starts when we're not even running? (Before exec or after exit) */
		fprintf(stderr, "Error: Unexpected subtest start (binary wasn't running)\n");
		return false;
	case STATE_SUBTEST_STARTED:
	case STATE_DYNAMIC_SUBTEST_STARTED:
	case STATE_BETWEEN_DYNAMIC_SUBTESTS:
		/*
		 * Subtest starts when the previous one was still
		 * running. Text-based parsing would figure that a
		 * resume happened, but we know the real deal with
		 * socket comms.
		 */
		snprintf(errmsg, sizeof(errmsg),
			 "\nrunner: Subtest %s already running when subtest %s starts. This is a test bug.\n",
			 context->current_subtest_name,
			 helper.subteststart.name);
		append_line(&context->errbuf, &context->errbuflen, errmsg);

		if (context->state == STATE_DYNAMIC_SUBTEST_STARTED ||
		    context->state == STATE_BETWEEN_DYNAMIC_SUBTESTS)
			comms_finish_dynamic_subtest(context);

		/* fallthrough */
	case STATE_BETWEEN_SUBTESTS:
		/* Already collecting for a subtest, finish it up */
		if (context->current_dynamic_subtest)
			comms_finish_dynamic_subtest(context);

		comms_finish_subtest(context);

		/* fallthrough */
	case STATE_AFTER_EXEC:
		comms_add_new_subtest(context, helper.subteststart.name);

		/* Subtest starting message is not in logs with socket comms, inject it manually */
		comms_inject_subtest_start_log(context, STARTING_SUBTEST, helper.subteststart.name);

		break;
	default:
		assert(false); /* unreachable */
	}

	context->state = STATE_SUBTEST_STARTED;

	return true;
}

static bool comms_handle_subtest_result(const struct runnerpacket *packet,
					runnerpacket_read_helper helper,
					void *userdata)
{
	struct comms_context *context = userdata;
	char errmsg[512];

	switch (context->state) {
	case STATE_INITIAL:
	case STATE_EXITED:
		/* Subtest result when we're not even running? (Before exec or after exit) */
		fprintf(stderr, "Error: Unexpected subtest result (binary wasn't running)\n");
		return false;
	case STATE_DYNAMIC_SUBTEST_STARTED:
		/*
		 * Subtest result when dynamic subtest is still
		 * running. Text-based parsing would consider that an
		 * incomplete, we're able to inject a warning.
		 */
		snprintf(errmsg, sizeof(errmsg),
			 "\nrunner: Dynamic subtest %s still running when subtest %s ended. This is a test bug.\n",
			 context->current_dynamic_subtest_name,
			 helper.subtestresult.name);
		append_line(&context->errbuf, &context->errbuflen, errmsg);
		comms_finish_dynamic_subtest(context);
		break;
	case STATE_BETWEEN_SUBTESTS:
		/* Subtest result without starting it, and we're already collecting logs for a previous test */
		comms_finish_subtest(context);
		comms_add_new_subtest(context, helper.subtestresult.name);
		break;
	case STATE_AFTER_EXEC:
		/* Subtest result without starting it, so comes from a fixture. We're not yet collecting logs for anything. */
		comms_add_new_subtest(context, helper.subtestresult.name);
		break;
	case STATE_SUBTEST_STARTED:
	case STATE_BETWEEN_DYNAMIC_SUBTESTS:
		/* Normal flow */
		break;
	default:
		assert(false); /* unreachable */
	}

	comms_inject_subtest_end_log(context,
				     SUBTEST_RESULT,
				     helper.subtestresult.name,
				     helper.subtestresult.result,
				     helper.subtestresult.timeused);

	/* Next subtest, if any, will begin its logs right after that result line */
	context->nextoutidx = context->outbuflen;
	context->nexterridx = context->errbuflen;

	/*
	 * Only store the actual result from the packet if we don't
	 * already have one. If we do, it's from an override.
	 */
	if (context->subtestresult == NULL) {
		const char *mappedresult;

		parse_result_string(helper.subtestresult.result,
				    strlen(helper.subtestresult.result),
				    &mappedresult, NULL);
		context->subtestresult = strdup(mappedresult);
	}

	context->state = STATE_BETWEEN_SUBTESTS;

	return true;
}

static bool comms_handle_dynamic_subtest_start(const struct runnerpacket *packet,
					       runnerpacket_read_helper helper,
					       void *userdata)
{
	struct comms_context *context = userdata;
	char errmsg[512];

	switch (context->state) {
	case STATE_INITIAL:
	case STATE_EXITED:
		/* Dynamic subtest starts when we're not even running? (Before exec or after exit) */
		fprintf(stderr, "Error: Unexpected dynamic subtest start (binary wasn't running)\n");
		return false;
	case STATE_AFTER_EXEC:
		/* Binary was running but a subtest wasn't. We don't know where to inject an error message. */
		fprintf(stderr, "Error: Unexpected dynamic subtest start (subtest wasn't running)\n");
		return false;
	case STATE_BETWEEN_SUBTESTS:
		/*
		 * Dynamic subtest starts when a subtest is not
		 * running. We can't know which subtest this dynamic
		 * subtest was supposed to be in. But we can inject a
		 * warn into the previous subtest.
		 */
		snprintf(errmsg, sizeof(errmsg),
			 "\nrunner: Dynamic subtest %s started when not inside a subtest. This is a test bug.\n",
			 helper.dynamicsubteststart.name);
		append_line(&context->errbuf, &context->errbuflen, errmsg);

		/* Leave the state as is and hope for the best */
		return true;
	case STATE_DYNAMIC_SUBTEST_STARTED:
		snprintf(errmsg, sizeof(errmsg),
			 "\nrunner: Dynamic subtest %s already running when dynamic subtest %s starts. This is a test bug.\n",
			 context->current_dynamic_subtest_name,
			 helper.dynamicsubteststart.name);
		append_line(&context->errbuf, &context->errbuflen, errmsg);

		/* fallthrough */
	case STATE_BETWEEN_DYNAMIC_SUBTESTS:
		comms_finish_dynamic_subtest(context);
		/* fallthrough */
	case STATE_SUBTEST_STARTED:
		comms_add_new_dynamic_subtest(context, helper.dynamicsubteststart.name);

		/* Dynamic subtest starting message is not in logs with socket comms, inject it manually */
		comms_inject_subtest_start_log(context, STARTING_DYNAMIC_SUBTEST, helper.dynamicsubteststart.name);

		break;
	default:
		assert(false); /* unreachable */
	}

	context->state = STATE_DYNAMIC_SUBTEST_STARTED;

	return true;
}

static bool comms_handle_dynamic_subtest_result(const struct runnerpacket *packet,
						runnerpacket_read_helper helper,
						void *userdata)
{
	struct comms_context *context = userdata;
	char errmsg[512];

	switch (context->state) {
	case STATE_INITIAL:
	case STATE_EXITED:
		/* Dynamic subtest result when we're not even running? (Before exec or after exit) */
		fprintf(stderr, "Error: Unexpected dynamic subtest result (binary wasn't running)\n");
		return false;
	case STATE_AFTER_EXEC:
		/* Binary was running but a subtest wasn't. We don't know where to inject an error message. */
		fprintf(stderr, "Error: Unexpected dynamic subtest result (subtest wasn't running)\n");
		return false;
	case STATE_BETWEEN_SUBTESTS:
		/*
		 * Dynamic subtest result when a subtest is not
		 * running. We can't know which subtest this dynamic
		 * subtest was supposed to be in. But we can inject a
		 * warn into the previous subtest.
		 */
		snprintf(errmsg, sizeof(errmsg),
			 "\nrunner: Dynamic subtest %s result when not inside a subtest. This is a test bug.\n",
			 helper.dynamicsubtestresult.name);
		append_line(&context->errbuf, &context->errbuflen, errmsg);

		/* Leave the state as is and hope for the best */
		return true;
	case STATE_BETWEEN_DYNAMIC_SUBTESTS:
		/*
		 * Result without starting. There's no
		 * skip_subtests_henceforth equivalent for dynamic
		 * subtests so this shouldn't happen, but we can
		 * handle it nevertheless.
		 */
		comms_finish_dynamic_subtest(context);
		/* fallthrough */
	case STATE_SUBTEST_STARTED:
		/* Result without starting, but we aren't collecting for a dynamic subtest yet */
		comms_add_new_dynamic_subtest(context, helper.dynamicsubtestresult.name);
		break;
	case STATE_DYNAMIC_SUBTEST_STARTED:
		/* Normal flow */
		break;
	default:
		assert(false); /* unreachable */
	}

	comms_inject_subtest_end_log(context,
				     DYNAMIC_SUBTEST_RESULT,
				     helper.dynamicsubtestresult.name,
				     helper.dynamicsubtestresult.result,
				     helper.dynamicsubtestresult.timeused);

	/* Next dynamic subtest, if any, will begin its logs right after that result line */
	context->nextdynoutidx = context->outbuflen;
	context->nextdynerridx = context->errbuflen;

	/*
	 * Only store the actual result from the packet if we don't
	 * already have one. If we do, it's from an override.
	 */
	if (context->dynamicsubtestresult == NULL) {
		const char *mappedresult;

		parse_result_string(helper.dynamicsubtestresult.result,
				    strlen(helper.dynamicsubtestresult.result),
				    &mappedresult, NULL);
		context->dynamicsubtestresult = strdup(mappedresult);
	}

	context->state = STATE_BETWEEN_DYNAMIC_SUBTESTS;

	return true;
}

static bool comms_handle_versionstring(const struct runnerpacket *packet,
				       runnerpacket_read_helper helper,
				       void *userdata)
{
	struct comms_context *context = userdata;

	free(context->igt_version);
	context->igt_version = strdup(helper.versionstring.text);

	return true;
}

static bool comms_handle_result_override(const struct runnerpacket *packet,
					 runnerpacket_read_helper helper,
					 void *userdata)
{
	struct comms_context *context = userdata;

	if (context->current_dynamic_subtest) {
		free(context->dynamicsubtestresult);
		context->dynamicsubtestresult = strdup(helper.resultoverride.result);
	}

	free(context->subtestresult);
	context->subtestresult = strdup(helper.resultoverride.result);

	return true;
}

static int fill_from_comms(int fd,
			   struct job_list_entry *entry,
			   struct subtest_list *subtests,
			   struct results *results)
{
	struct comms_context context = {};
	struct comms_visitor visitor = {
		.log = comms_handle_log,
		.exec = comms_handle_exec,
		.exit = comms_handle_exit,
		.subtest_start = comms_handle_subtest_start,
		.subtest_result = comms_handle_subtest_result,
		.dynamic_subtest_start = comms_handle_dynamic_subtest_start,
		.dynamic_subtest_result = comms_handle_dynamic_subtest_result,
		.versionstring = comms_handle_versionstring,
		.result_override = comms_handle_result_override,

		.userdata = &context,
	};
	char piglit_name[256];
	int ret = COMMSPARSE_EMPTY;

	if (fd < 0)
		return COMMSPARSE_EMPTY;

	context.entry = entry;
	context.binary = entry->binary;
	generate_piglit_name(entry->binary, NULL, piglit_name, sizeof(piglit_name));
	context.binaryruntimeobj = get_or_create_json_object(results->runtimes, piglit_name);
	context.results = results;
	context.subtests = subtests;

	ret = comms_read_dump(fd, &visitor);

	if (context.current_dynamic_subtest != NULL)
		comms_finish_dynamic_subtest(&context);
	if (context.current_test != NULL)
		comms_finish_subtest(&context);
	comms_free_context(&context);

	return ret;
}

static bool result_is_requested(struct job_list_entry *entry,
				const char *subtestname,
				const char *dynamic_name)
{
	char entryname[512];
	size_t i;

	if (dynamic_name)
		snprintf(entryname, sizeof(entryname) - 1, "%s@%s", subtestname, dynamic_name);
	else
		strncpy(entryname, subtestname, sizeof(entryname) - 1);

	for (i = 0; i < entry->subtest_count; i++) {
		if (!strcmp(entry->subtests[i], entryname))
			return true;
	}

	return false;
}

static void prune_subtests(struct settings *settings,
			   struct job_list_entry *entry,
			   struct subtest_list *subtests,
			   struct json_object *tests)
{
	char piglit_name[256];
	char dynamic_piglit_name[256];
	size_t i, k;

	if (settings->prune_mode == PRUNE_KEEP_ALL)
		return;

	for (i = 0; i < subtests->size; i++) {
		generate_piglit_name(entry->binary, subtests->subs[i].name, piglit_name, sizeof(piglit_name));

		if (settings->prune_mode == PRUNE_KEEP_DYNAMIC) {
			if (subtests->subs[i].dynamic_size)
				json_object_object_del(tests, piglit_name);

			continue;
		}

		assert(settings->prune_mode == PRUNE_KEEP_SUBTESTS || settings->prune_mode == PRUNE_KEEP_REQUESTED);

		if (settings->prune_mode == PRUNE_KEEP_REQUESTED &&
		    !result_is_requested(entry, subtests->subs[i].name, NULL)) {
			json_object_object_del(tests, piglit_name);
		}

		for (k = 0; k < subtests->subs[i].dynamic_size; k++) {
			if (settings->prune_mode == PRUNE_KEEP_SUBTESTS ||
			    (settings->prune_mode == PRUNE_KEEP_REQUESTED &&
			     !result_is_requested(entry, subtests->subs[i].name, subtests->subs[i].dynamic_names[k]))) {
				generate_piglit_name_for_dynamic(piglit_name, subtests->subs[i].dynamic_names[k],
								 dynamic_piglit_name, sizeof(dynamic_piglit_name));
				json_object_object_del(tests, dynamic_piglit_name);
			}
		}
	}
}

static bool stderr_contains_warnings(const char *beg, const char *end)
{
	struct match_needle needles[] = {
		{ STARTING_SUBTEST, NULL },
		{ SUBTEST_RESULT, is_subtest_result_line },
		{ STARTING_DYNAMIC_SUBTEST, NULL },
		{ DYNAMIC_SUBTEST_RESULT, is_subtest_result_line },
		{ NULL, NULL },
	};
	struct matches matches;
	size_t i = 0;
	bool found = false;

	matches = find_matches(beg, end, needles);

	while (i < matches.size) {
		if (matches.items[i].where != beg) {
			found = true;
			break;
		}
		beg = next_line(beg, end);
		i++;
	}

	free_matches(&matches);

	return found;
}

static bool json_field_has_data(struct json_object *obj, const char *key)
{
	struct json_object *field;

	if (json_object_object_get_ex(obj, key, &field))
		return strcmp(json_object_get_string(field), "");

	return false;
}

static void override_completely_empty_results(struct json_object *obj)
{
	if (json_field_has_data(obj, "out") ||
	    json_field_has_data(obj, "err") ||
	    json_field_has_data(obj, "dmesg"))
		return;

	json_object_object_add(obj, "out",
			       json_object_new_string("This test didn't produce any output. "
						      "The machine probably rebooted ungracefully.\n"));
	set_result(obj, "incomplete");
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
	    stderr_contains_warnings(errtext, errtext + strlen(errtext))) {
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

	override_completely_empty_results(obj);
}

static void override_results(char *binary,
			     struct subtest_list *subtests,
			     struct json_object *tests)
{
	struct json_object *obj;
	char piglit_name[256];
	char dynamic_piglit_name[256];
	size_t i, k;

	if (subtests->size == 0) {
		generate_piglit_name(binary, NULL, piglit_name, sizeof(piglit_name));
		obj = get_or_create_json_object(tests, piglit_name);
		override_result_single(obj);
		return;
	}

	for (i = 0; i < subtests->size; i++) {
		generate_piglit_name(binary, subtests->subs[i].name, piglit_name, sizeof(piglit_name));
		obj = get_or_create_json_object(tests, piglit_name);
		override_result_single(obj);

		for (k = 0; k < subtests->subs[i].dynamic_size; k++) {
			generate_piglit_name_for_dynamic(piglit_name, subtests->subs[i].dynamic_names[k],
							 dynamic_piglit_name, sizeof(dynamic_piglit_name));
			obj = get_or_create_json_object(tests, dynamic_piglit_name);
			override_result_single(obj);
		}
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
	json_object_object_add(obj, "abort", json_object_new_int(0));
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
			  struct subtest_list *subtests,
			  struct results *results)
{
	struct json_object *test, *resultobj, *emptystrtotal, *roottotal, *binarytotal;
	char piglit_name[256];
	char dynamic_piglit_name[256];
	const char *result;
	size_t i, k;

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
		generate_piglit_name(binary, subtests->subs[i].name, piglit_name, sizeof(piglit_name));

		if (json_object_object_get_ex(results->tests, piglit_name, &test)) {
			if (!json_object_object_get_ex(test, "result", &resultobj)) {
				fprintf(stderr, "Warning: No results set for %s\n", piglit_name);
				return;
			}
			result = json_object_get_string(resultobj);
			add_result_to_totals(emptystrtotal, result);
			add_result_to_totals(roottotal, result);
			add_result_to_totals(binarytotal, result);
		}

		for (k = 0; k < subtests->subs[i].dynamic_size; k++) {
			generate_piglit_name_for_dynamic(piglit_name, subtests->subs[i].dynamic_names[k],
							 dynamic_piglit_name, sizeof(dynamic_piglit_name));

			if (json_object_object_get_ex(results->tests, dynamic_piglit_name, &test)) {
				if (!json_object_object_get_ex(test, "result", &resultobj)) {
					fprintf(stderr, "Warning: No results set for %s\n", dynamic_piglit_name);
					return;
				}
				result = json_object_get_string(resultobj);
				add_result_to_totals(emptystrtotal, result);
				add_result_to_totals(roottotal, result);
				add_result_to_totals(binarytotal, result);
			}
		}
	}
}

static bool parse_test_directory(int dirfd,
				 struct job_list_entry *entry,
				 struct settings *settings,
				 struct results *results)
{
	int fds[_F_LAST];
	struct subtest_list subtests = {};
	bool status = true;
	int commsparsed;

	if (!open_output_files(dirfd, fds, false)) {
		fprintf(stderr, "Error opening output files\n");
		return false;
	}

	/*
	 * Get test output from socket comms if it exists, otherwise
	 * parse stdout/stderr
	 */
	commsparsed = fill_from_comms(fds[_F_SOCKET], entry, &subtests, results);
	if (commsparsed == COMMSPARSE_ERROR) {
		fprintf(stderr, "Error parsing output files (comms)\n");
		status = false;
		goto parse_output_end;
	}

	if (commsparsed == COMMSPARSE_EMPTY) {
		/*
		 * fill_from_journal fills the subtests struct and
		 * adds timeout results where applicable.
		 */
		fill_from_journal(fds[_F_JOURNAL], entry, &subtests, results);

		if (!fill_from_output(fds[_F_OUT], entry->binary, "out", &subtests, results->tests) ||
		    !fill_from_output(fds[_F_ERR], entry->binary, "err", &subtests, results->tests)) {
			fprintf(stderr, "Error parsing output files (out.txt, err.txt)\n");
			status = false;
			goto parse_output_end;
		}
	}

	if (!fill_from_dmesg(fds[_F_DMESG], settings, entry->binary, &subtests, results->tests)) {
		fprintf(stderr, "Error parsing output files (dmesg.txt)\n");
		status = false;
		goto parse_output_end;
	}

	override_results(entry->binary, &subtests, results->tests);
	prune_subtests(settings, entry, &subtests, results->tests);

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
	struct subtest_list subtests = {};
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
				       new_escaped_json_string(buf, r));
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
		struct subtest_list abortsub = {};
		struct json_object *aborttest = get_or_create_json_object(results.tests, piglit_name);
		ssize_t s;

		add_subtest(&abortsub, strdup("aborted"));

		s = read(fd, buf, sizeof(buf));

		json_object_object_add(aborttest, "out",
				       new_escaped_json_string(buf, s));
		json_object_object_add(aborttest, "err",
				       json_object_new_string(""));
		json_object_object_add(aborttest, "dmesg",
				       json_object_new_string(""));
		json_object_object_add(aborttest, "result",
				       json_object_new_string("fail"));

		add_to_totals("runner", &abortsub, &results);

		free_subtests(&abortsub);
		close(fd);
	}

	clear_settings(&settings);
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

	if (json_string == NULL) {
		fprintf(stderr, "resultgen: Failed to create json representation of the results.\n");
		fprintf(stderr, "           This usually means that the results are too big\n");
		fprintf(stderr, "           to fit in the memory as the text representation\n");
		fprintf(stderr, "           is being created.\n\n");
		fprintf(stderr, "           Either something was spamming the logs or your\n");
		fprintf(stderr, "           system is very low on free mem.\n");

		close(resultsfd);
		return false;
	}

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
