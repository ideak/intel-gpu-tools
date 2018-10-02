#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "igt.h"

#include "settings.h"
#include "job_list.h"
#include "executor.h"

static char testdatadir[] = TESTDATA_DIRECTORY;

static void igt_assert_eqstr(char *one, char *two)
{
	if (one == NULL && two == NULL)
		return;

	igt_assert_f(one != NULL && two != NULL, "Strings differ (one is NULL): %s vs %s\n", one, two);

	igt_assert_f(!strcmp(one, two), "Strings differ: '%s' vs '%s'\n", one, two);
}

static void debug_print_executions(struct job_list *list)
{
	size_t i;
	int k;

	igt_debug("Executions:\n");
	for (i = 0; i < list->size; i++) {
		struct job_list_entry *entry = &list->entries[i];
		igt_debug(" %s\n", entry->binary);
		for (k = 0; k < entry->subtest_count; ++k) {
			igt_debug("  %s\n", entry->subtests[k]);
		}
	}

}

static char *dump_file(int dirfd, char *name)
{
	int fd = openat(dirfd, name, O_RDONLY);
	ssize_t s;
	char *buf = malloc(256);

	if (fd < 0) {
		free(buf);
		return NULL;
	}

	s = read(fd, buf, 255);
	close(fd);

	if (s < 0) {
		free(buf);
		return NULL;
	}

	buf[s] = '\0';
	return buf;
}

static void job_list_filter_test(char *name, char *filterarg1, char *filterarg2,
				 size_t expected_normal, size_t expected_multiple)
{
	int multiple;
	struct settings settings;

	igt_fixture
		init_settings(&settings);

	for (multiple = 0; multiple < 2; multiple++) {
		igt_subtest_f("job-list-filters-%s-%s", name, multiple ? "multiple" : "normal") {
			struct job_list list;
			char *argv[] = { "runner",
					 /* Ugly but does the trick */
					 multiple ? "--multiple-mode" : "--sync",
					 filterarg1, filterarg2,
					 testdatadir,
					 "path-to-results",
			};
			bool success = false;
			size_t size;

			init_job_list(&list);
			igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));

			success = create_job_list(&list, &settings);
			size = list.size;

			if (success)
				debug_print_executions(&list);

			free_job_list(&list);

			igt_assert_f(success, "Job list creation failed\n");
			igt_assert_eq(size, multiple ? expected_multiple : expected_normal);
		}
	}

	igt_fixture
		free_settings(&settings);
}

static void clear_directory_fd(int dirfd)
{
	DIR* d;
	struct dirent *dirent;

	d = fdopendir(dirfd);

	if (dirfd < 0 || d == NULL) {
		return;
	}

	while ((dirent = readdir(d)) != NULL) {
		if (strcmp(dirent->d_name, ".") &&
		    strcmp(dirent->d_name, "..")) {
			if (dirent->d_type == DT_REG) {
				unlinkat(dirfd, dirent->d_name, 0);
			} else if (dirent->d_type == DT_DIR) {
				clear_directory_fd(openat(dirfd, dirent->d_name, O_DIRECTORY | O_RDONLY));
				unlinkat(dirfd, dirent->d_name, AT_REMOVEDIR);
			}
		}
	}

	closedir(d);
}

static void clear_directory(char *name)
{
	int dirfd = open(name, O_DIRECTORY | O_RDONLY);
	clear_directory_fd(dirfd);
	rmdir(name);
}

static void assert_settings_equal(struct settings *one, struct settings *two)
{
	/*
	 * Regex lists are not serialized, and thus won't be compared
	 * here.
	 */
	igt_assert_eq(one->abort_on_error, two->abort_on_error);
	igt_assert_eqstr(one->test_list, two->test_list);
	igt_assert_eqstr(one->name, two->name);
	igt_assert_eq(one->dry_run, two->dry_run);
	igt_assert_eq(one->sync, two->sync);
	igt_assert_eq(one->log_level, two->log_level);
	igt_assert_eq(one->overwrite, two->overwrite);
	igt_assert_eq(one->multiple_mode, two->multiple_mode);
	igt_assert_eq(one->inactivity_timeout, two->inactivity_timeout);
	igt_assert_eq(one->use_watchdog, two->use_watchdog);
	igt_assert_eqstr(one->test_root, two->test_root);
	igt_assert_eqstr(one->results_path, two->results_path);
	igt_assert_eq(one->piglit_style_dmesg, two->piglit_style_dmesg);
}

static void assert_job_list_equal(struct job_list *one, struct job_list *two)
{
	size_t i, k;

	igt_assert_eq(one->size, two->size);

	for (i = 0; i < one->size; i++) {
		struct job_list_entry *eone = &one->entries[i];
		struct job_list_entry *etwo = &two->entries[i];

		igt_assert_eqstr(eone->binary, etwo->binary);
		igt_assert_eq(eone->subtest_count, etwo->subtest_count);

		for (k = 0; k < eone->subtest_count; k++) {
			igt_assert_eqstr(eone->subtests[k], etwo->subtests[k]);
		}
	}
}

static void assert_execution_created(int dirfd, char *name)
{
	int fd;

	igt_assert_f((fd = openat(dirfd, name, O_RDONLY)) >= 0,
		     "Execute didn't create %s\n", name);
	close(fd);
}

static void assert_execution_results_exist(int dirfd)
{
	assert_execution_created(dirfd, "journal.txt");
	assert_execution_created(dirfd, "out.txt");
	assert_execution_created(dirfd, "err.txt");
	assert_execution_created(dirfd, "dmesg.txt");
}

igt_main
{
	struct settings settings;

	igt_fixture
		init_settings(&settings);

	igt_subtest("default-settings") {
		char *argv[] = { "runner",
				 "test-root-dir",
				 "path-to-results",
		};

		igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));

		igt_assert(!settings.abort_on_error);
		igt_assert(!settings.test_list);
		igt_assert_eqstr(settings.name, "path-to-results");
		igt_assert(!settings.dry_run);
		igt_assert_eq(settings.include_regexes.size, 0);
		igt_assert_eq(settings.exclude_regexes.size, 0);
		igt_assert(!settings.sync);
		igt_assert_eq(settings.log_level, LOG_LEVEL_NORMAL);
		igt_assert(!settings.overwrite);
		igt_assert(!settings.multiple_mode);
		igt_assert_eq(settings.inactivity_timeout, 0);
		igt_assert(!settings.use_watchdog);
		igt_assert(strstr(settings.test_root, "test-root-dir") != NULL);
		igt_assert(strstr(settings.results_path, "path-to-results") != NULL);
		igt_assert(!settings.piglit_style_dmesg);
	}

	igt_subtest_group {
		char *cwd;
		char *path;

		igt_fixture {
			igt_require((cwd = realpath(".", NULL)) != NULL);
			path = NULL;
		}

		igt_subtest("absolute-path-converter") {
			char paths[][15] = { "simple-name", "foo/bar", "." };
			size_t i;

			for (i = 0; i < ARRAY_SIZE(paths); i++) {
				free(path);
				path = absolute_path(paths[i]);

				igt_assert(path[0] == '/');
				igt_debug("Got path %s for %s\n", path, paths[i]);
				igt_assert(strstr(path, cwd) == path);
				if (strcmp(paths[i], ".")) {
					igt_assert(strstr(path, paths[i]) != NULL);
				}
			}
		}

		igt_fixture {
			free(cwd);
			free(path);
		}
	}

	igt_subtest_group {
		char dirname[] = "tmpdirXXXXXX";
		char tmptestlist[] = "tmp.testlist";
		char pathtotestlist[64];
		char *path;

		igt_fixture {
			int dirfd, fd;

			path = NULL;

			igt_require(mkdtemp(dirname) != NULL);
			igt_require((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0);
			igt_require((fd = openat(dirfd, tmptestlist, O_CREAT | O_EXCL | O_WRONLY, 0660)) >= 0);
			close(fd);
			close(dirfd);

			strcpy(pathtotestlist, dirname);
			strcat(pathtotestlist, "/");
			strcat(pathtotestlist, tmptestlist);
		}

		igt_subtest("absolute-path-usage") {
			char *argv[] = { "runner",
					 "--test-list", pathtotestlist,
					 testdatadir,
					 dirname,
			};

			igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));

			path = realpath(testdatadir, NULL);
			igt_assert(path != NULL);
			igt_assert_eqstr(settings.test_root, path);
			free(path);
			path = realpath(dirname, NULL);
			igt_assert(path != NULL);
			igt_assert_eqstr(settings.results_path, path);
			free(path);
			path = realpath(pathtotestlist, NULL);
			igt_assert(path != NULL);
			igt_assert_eqstr(settings.test_list, path);
		}

		igt_fixture {
			int dirfd;

			igt_require((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0);
			unlinkat(dirfd, tmptestlist, 0);
			close(dirfd);
			rmdir(dirname);

			free(path);
		}
	}

	igt_subtest("environment-overrides-test-root-flag") {
		char *argv[] = { "runner",
				 "test-root-dir",
				 "path-to-results",
		};

		setenv("IGT_TEST_ROOT", testdatadir, 1);
		igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));

		igt_assert(!settings.abort_on_error);
		igt_assert(!settings.test_list);
		igt_assert_eqstr(settings.name, "path-to-results");
		igt_assert(!settings.dry_run);
		igt_assert_eq(settings.include_regexes.size, 0);
		igt_assert_eq(settings.exclude_regexes.size, 0);
		igt_assert(!settings.sync);
		igt_assert_eq(settings.log_level, LOG_LEVEL_NORMAL);
		igt_assert(!settings.overwrite);
		igt_assert(!settings.multiple_mode);
		igt_assert_eq(settings.inactivity_timeout, 0);
		igt_assert(!settings.use_watchdog);
		igt_assert(strstr(settings.test_root, testdatadir) != NULL);
		igt_assert(strstr(settings.results_path, "path-to-results") != NULL);
		igt_assert(!settings.piglit_style_dmesg);
	}

	igt_fixture {
		unsetenv("IGT_TEST_ROOT");
	}

	igt_subtest("parse-all-settings") {
		char *argv[] = { "runner",
				 "-n", "foo",
				 "--abort-on-monitored-error",
				 "--test-list", "path-to-test-list",
				 "--ignore-missing",
				 "--dry-run",
				 "-t", "pattern1",
				 "-t", "pattern2",
				 "-x", "xpattern1",
				 "-x", "xpattern2",
				 "-s",
				 "-l", "verbose",
				 "--overwrite",
				 "--multiple-mode",
				 "--inactivity-timeout", "27",
				 "--use-watchdog",
				 "--piglit-style-dmesg",
				 "test-root-dir",
				 "path-to-results",
		};

		igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));

		igt_assert(settings.abort_on_error);
		igt_assert(strstr(settings.test_list, "path-to-test-list") != NULL);
		igt_assert_eqstr(settings.name, "foo");
		igt_assert(settings.dry_run);
		igt_assert_eq(settings.include_regexes.size, 2);
		igt_assert_eqstr(settings.include_regexes.regex_strings[0], "pattern1");
		igt_assert_eqstr(settings.include_regexes.regex_strings[1], "pattern2");
		igt_assert_eq(settings.exclude_regexes.size, 2);
		igt_assert_eqstr(settings.exclude_regexes.regex_strings[0], "xpattern1");
		igt_assert_eqstr(settings.exclude_regexes.regex_strings[1], "xpattern2");
		igt_assert(settings.sync);
		igt_assert_eq(settings.log_level, LOG_LEVEL_VERBOSE);
		igt_assert(settings.overwrite);
		igt_assert(settings.multiple_mode);
		igt_assert_eq(settings.inactivity_timeout, 27);
		igt_assert(settings.use_watchdog);
		igt_assert(strstr(settings.test_root, "test-root-dir") != NULL);
		igt_assert(strstr(settings.results_path, "path-to-results") != NULL);
		igt_assert(settings.piglit_style_dmesg);
	}

	igt_subtest("invalid-option") {
		char *argv[] = { "runner",
				 "--no-such-option",
				 "test-root-dir",
				 "results-path",
		};

		igt_assert(!parse_options(ARRAY_SIZE(argv), argv, &settings));
	}

	igt_subtest("paths-missing") {
		char *argv[] = { "runner",
				 "-o",
		};
		igt_assert(!parse_options(ARRAY_SIZE(argv), argv, &settings));
	}

	igt_subtest("log-levels") {
		char *argv[] = { "runner",
				 "-l", "normal",
				 "test-root-dir",
				 "results-path",
		};

		igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));
		igt_assert_eq(settings.log_level, LOG_LEVEL_NORMAL);

		argv[2] = "quiet";
		igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));
		igt_assert_eq(settings.log_level, LOG_LEVEL_QUIET);

		argv[2] = "verbose";
		igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));
		igt_assert_eq(settings.log_level, LOG_LEVEL_VERBOSE);
	}

	igt_subtest("parse-clears-old-data") {
		char *argv[] = { "runner",
				 "-n", "foo",
				 "--dry-run",
				 "test-root-dir",
				 "results-path",
		};

		igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));

		igt_assert_eqstr(settings.name, "foo");
		igt_assert(settings.dry_run);
		igt_assert(!settings.test_list);
		igt_assert(!settings.sync);

		argv[1] = "--test-list";
		argv[3] = "--sync";

		igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));

		igt_assert_eqstr(settings.name, "results-path");
		igt_assert(!settings.dry_run);
		igt_assert(strstr(settings.test_list, "foo") != NULL);
		igt_assert(settings.sync);
	}

	igt_subtest_group {
		char filename[] = "tmplistXXXXXX";
		int fd = -1;

		igt_fixture {
			igt_require((fd = mkstemp(filename)) >= 0);
			close(fd);
		}

		igt_subtest("validate-ok") {
			char *argv[] = { "runner",
					 "--test-list", filename,
					 testdatadir,
					 "path-to-results",
			};

			igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));

			igt_assert(validate_settings(&settings));
		}

		igt_fixture {
			unlink(filename);
		}
	}

	igt_subtest("validate-no-test-list") {
		char *nosuchfile = "no-such-file";
		char *argv[] = { "runner",
				 "--test-list", nosuchfile,
				 testdatadir,
				 "path-to-results",
		};

		igt_assert_lt(open(nosuchfile, O_RDONLY), 0);
		igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));

		igt_assert(!validate_settings(&settings));
	}

	igt_subtest_group {
		char dirname[] = "tmpdirXXXXXX";
		struct job_list list;

		igt_fixture {
			igt_require(mkdtemp(dirname) != NULL);
			init_job_list(&list);
		}

		igt_subtest("job-list-no-test-list-txt") {
			char *argv[] = { "runner",
					 dirname,
					 "path-to-results",
			};

			igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));

			igt_assert(!create_job_list(&list, &settings));
		}

		igt_fixture {
			rmdir(dirname);
			free_job_list(&list);
		}
	}

	job_list_filter_test("nofilters", "-n", "placeholderargs", 5, 3);
	job_list_filter_test("binary-include", "-t", "successtest", 2, 1);
	job_list_filter_test("binary-exclude", "-x", "successtest", 3, 2);
	job_list_filter_test("subtest-include", "-t", "first-subtest", 1, 1);
	job_list_filter_test("subtest-exclude", "-x", "second-subtest", 4, 3);
	job_list_filter_test("piglit-names", "-t", "igt@successtest", 2, 1);
	job_list_filter_test("piglit-names-subtest", "-t", "igt@successtest@first", 1, 1);

	igt_subtest_group {
		char filename[] = "tmplistXXXXXX";
		char testlisttext[] = "igt@successtest@first-subtest\n"
			"igt@successtest@second-subtest\n"
			"igt@nosubtests\n";
		int fd = -1, multiple;
		struct job_list list;

		igt_fixture {
			igt_require((fd = mkstemp(filename)) >= 0);
			igt_require(write(fd, testlisttext, strlen(testlisttext)) == strlen(testlisttext));
			close(fd);
			init_job_list(&list);
		}

		for (multiple = 0; multiple < 2; multiple++) {
			igt_subtest_f("job-list-testlist-%s", multiple ? "multiple" : "normal") {
				char *argv[] = { "runner",
						 "--test-list", filename,
						 multiple ? "--multiple-mode" : "--sync",
						 testdatadir,
						 "path-to-results",
				};

				igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));
				igt_assert(create_job_list(&list, &settings));

				igt_assert_eq(list.size, multiple ? 2 : 3);

				igt_assert_eqstr(list.entries[0].binary, "successtest");
				if (!multiple) igt_assert_eqstr(list.entries[1].binary, "successtest");
				igt_assert_eqstr(list.entries[multiple ? 1 : 2].binary, "nosubtests");

				igt_assert_eq(list.entries[0].subtest_count, multiple ? 2 : 1);
				igt_assert_eq(list.entries[1].subtest_count, multiple ? 0 : 1);
				if (!multiple) igt_assert_eq(list.entries[2].subtest_count, 0);

				igt_assert_eqstr(list.entries[0].subtests[0], "first-subtest");
				igt_assert_eqstr(list.entries[multiple ? 0 : 1].subtests[multiple ? 1 : 0], "second-subtest");
			}

			igt_subtest_f("job-list-testlist-filtered-%s", multiple ? "multiple" : "normal") {
				char *argv[] = { "runner",
						 "--test-list", filename,
						 multiple ? "--multiple-mode" : "--sync",
						 "-t", "successtest",
						 "-x", "first",
						 testdatadir,
						 "path-to-results",
				};

				igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));
				igt_assert(create_job_list(&list, &settings));

				igt_assert_eq(list.size, 1);
				igt_assert_eqstr(list.entries[0].binary, "successtest");

				igt_assert_eq(list.entries[0].subtest_count, 1);
				igt_assert_eqstr(list.entries[0].subtests[0], "second-subtest");
			}
		}

		igt_fixture {
			unlink(filename);
			free_job_list(&list);
		}
	}

	igt_subtest_group {
		char dirname[] = "tmpdirXXXXXX";
		int dirfd = -1, fd = -1;
		struct settings cmp_settings;

		igt_fixture {
			igt_require(mkdtemp(dirname) != NULL);
			rmdir(dirname);
			init_settings(&cmp_settings);
		}

		igt_subtest("settings-serialize") {
			char *argv[] = { "runner",
					 "-n", "foo",
					 "--abort-on-monitored-error",
					 "--test-list", "path-to-test-list",
					 "--ignore-missing",
					 "--dry-run",
					 "-t", "pattern1",
					 "-t", "pattern2",
					 "-x", "xpattern1",
					 "-x", "xpattern2",
					 "-s",
					 "-l", "verbose",
					 "--overwrite",
					 "--multiple-mode",
					 "--inactivity-timeout", "27",
					 "--use-watchdog",
					 "--piglit-style-dmesg",
					 testdatadir,
					 dirname,
			};

			igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));

			igt_assert(serialize_settings(&settings));

			dirfd = open(dirname, O_DIRECTORY, O_RDONLY);
			igt_assert_f(dirfd >= 0, "Serialization did not create the results directory\n");

			igt_assert_f((fd = openat(dirfd, "metadata.txt", O_RDONLY)),
				     "Opening %s/metadata.txt failed\n", dirname);
			close(fd);

			igt_assert_f(read_settings(&cmp_settings, dirfd), "Reading settings failed\n");
			assert_settings_equal(&settings, &cmp_settings);
		}

		igt_fixture {
			close(fd);
			close(dirfd);
			clear_directory(dirname);
			free_settings(&cmp_settings);
		}
	}

	igt_subtest_group {
		char dirname[] = "tmpdirXXXXXX";
		int dirfd = -1, fd = -1;
		struct job_list list, cmp_list;
		int multiple;

		igt_fixture {
			init_job_list(&list);
			init_job_list(&cmp_list);
			igt_require(mkdtemp(dirname) != NULL);
			rmdir(dirname);
		}

		for (multiple = 0; multiple < 2; multiple++) {
			igt_subtest_f("job-list-serialize-%s", multiple ? "multiple" : "normal") {
				char *argv[] = { "runner",
						 /* Ugly */
						 multiple ? "--multiple-mode" : "--sync",
						 testdatadir,
						 dirname,
				};

				igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));
				igt_assert(create_job_list(&list, &settings));

				igt_assert(serialize_settings(&settings));
				igt_assert(serialize_job_list(&list, &settings));

				dirfd = open(dirname, O_DIRECTORY, O_RDONLY);
				igt_assert_f(dirfd >= 0, "Serialization did not create the results directory\n");

				igt_assert_f((fd = openat(dirfd, "joblist.txt", O_RDONLY)) >= 0,
					     "Opening %s/joblist.txt failed\n", dirname);
				close(fd);
				fd = -1;

				igt_assert_f(read_job_list(&cmp_list, dirfd), "Reading job list failed\n");
				assert_job_list_equal(&list, &cmp_list);
			}

			igt_fixture {
				close(fd);
				close(dirfd);
				clear_directory(dirname);
				free_job_list(&cmp_list);
				free_job_list(&list);
			}
		}
	}

	igt_subtest_group {
		char dirname[] = "tmpdirXXXXXX";
		struct job_list list;
		int dirfd = -1, fd = -1;

		igt_fixture {
			init_job_list(&list);
			igt_require(mkdtemp(dirname) != NULL);
			rmdir(dirname);
		}

		igt_subtest("execute-initialize-new-run") {
			struct execute_state state;
			char *argv[] = { "runner",
					 testdatadir,
					 dirname,
			};

			igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));
			igt_assert(create_job_list(&list, &settings));

			igt_assert(initialize_execute_state(&state, &settings, &list));

			igt_assert_eq(state.next, 0);
			igt_assert_eq(list.size, 5);
			igt_assert_f((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0,
				     "Execute state initialization didn't create the results directory.\n");
			igt_assert_f((fd = openat(dirfd, "metadata.txt", O_RDONLY)) >= 0,
				     "Execute state initialization didn't serialize settings.\n");
			close(fd);
			igt_assert_f((fd = openat(dirfd, "joblist.txt", O_RDONLY)) >= 0,
				     "Execute state initialization didn't serialize the job list.\n");
			close(fd);
			igt_assert_f((fd = openat(dirfd, "journal.txt", O_RDONLY)) < 0,
				     "Execute state initialization created a journal.\n");
			igt_assert_f((fd = openat(dirfd, "uname.txt", O_RDONLY)) < 0,
				     "Execute state initialization created uname.txt.\n");
		}

		igt_fixture {
			close(fd);
			close(dirfd);
			clear_directory(dirname);
			free_job_list(&list);
		}
	}

	igt_subtest_group {
		char dirname[] = "tmpdirXXXXXX";
		struct job_list list;
		int dirfd = -1, subdirfd = -1, fd = -1;

		igt_fixture {
			init_job_list(&list);
			igt_require(mkdtemp(dirname) != NULL);
		}

		igt_subtest("execute-initialize-subtest-started") {
			struct execute_state state;
			char *argv[] = { "runner",
					 "--multiple-mode",
					 "-t", "successtest",
					 testdatadir,
					 dirname,
			};
			char journaltext[] = "first-subtest\n";
			char excludestring[] = "!first-subtest";

			igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));
			igt_assert(create_job_list(&list, &settings));
			igt_assert(list.size == 1);
			igt_assert(list.entries[0].subtest_count == 0);

			igt_assert(serialize_settings(&settings));
			igt_assert(serialize_job_list(&list, &settings));

			igt_assert((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0);
			igt_assert(mkdirat(dirfd, "0", 0770) == 0);
			igt_assert((subdirfd = openat(dirfd, "0", O_DIRECTORY | O_RDONLY)) >= 0);
			igt_assert((fd = openat(subdirfd, "journal.txt", O_CREAT | O_WRONLY | O_EXCL, 0660)) >= 0);
			igt_assert(write(fd, journaltext, strlen(journaltext)) == strlen(journaltext));

			free_job_list(&list);
			free_settings(&settings);
			igt_assert(initialize_execute_state_from_resume(dirfd, &state, &settings, &list));

			igt_assert_eq(state.next, 0);
			igt_assert_eq(list.size, 1);
			igt_assert_eq(list.entries[0].subtest_count, 2);
			igt_assert_eqstr(list.entries[0].subtests[0], "*");
			igt_assert_eqstr(list.entries[0].subtests[1], excludestring);
		}

		igt_fixture {
			close(fd);
			close(subdirfd);
			close(dirfd);
			clear_directory(dirname);
			free_job_list(&list);
		}
	}

	igt_subtest_group {
		char dirname[] = "tmpdirXXXXXX";
		struct job_list list;
		int dirfd = -1, subdirfd = -1, fd = -1;

		igt_fixture {
			init_job_list(&list);
			igt_require(mkdtemp(dirname) != NULL);
		}

		igt_subtest("execute-initialize-subtests-complete") {
			struct execute_state state;
			char *argv[] = { "runner",
					 "--multiple-mode",
					 testdatadir,
					 dirname,
			};
			char journaltext[] = "first-subtest\nsecond-subtest\nexit:0\n";

			igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));
			igt_assert(create_job_list(&list, &settings));
			igt_assert(list.size == 3);

			if (!strcmp(list.entries[0].binary, "no-subtests")) {
				struct job_list_entry tmp = list.entries[0];
				list.entries[0] = list.entries[1];
				list.entries[1] = tmp;
			}

			igt_assert(list.entries[0].subtest_count == 0);

			igt_assert(serialize_settings(&settings));
			igt_assert(serialize_job_list(&list, &settings));

			igt_assert_lte(0, dirfd = open(dirname, O_DIRECTORY | O_RDONLY));
			igt_assert_eq(mkdirat(dirfd, "0", 0770), 0);
			igt_assert((subdirfd = openat(dirfd, "0", O_DIRECTORY | O_RDONLY)) >= 0);
			igt_assert_lte(0, fd = openat(subdirfd, "journal.txt", O_CREAT | O_WRONLY | O_EXCL, 0660));
			igt_assert_eq(write(fd, journaltext, sizeof(journaltext)), sizeof(journaltext));

			free_job_list(&list);
			free_settings(&settings);
			igt_assert(initialize_execute_state_from_resume(dirfd, &state, &settings, &list));

			igt_assert_eq(state.next, 1);
			igt_assert_eq(list.size, 3);
		}

		igt_fixture {
			close(fd);
			close(subdirfd);
			close(dirfd);
			clear_directory(dirname);
			free_job_list(&list);
		}
	}

	igt_subtest_group {
		struct job_list list;
		int dirfd = -1, subdirfd = -1, fd = -1;
		int multiple;

		igt_fixture {
			init_job_list(&list);
		}

		for (multiple = 0; multiple < 2; multiple++) {
			char dirname[] = "tmpdirXXXXXX";

			igt_fixture {
				igt_require(mkdtemp(dirname) != NULL);
				rmdir(dirname);
			}

			igt_subtest_f("execute-subtests-%s", multiple ? "multiple" : "normal") {
				struct execute_state state;
				char *argv[] = { "runner",
						 multiple ? "--multiple-mode" : "--sync",
						 "-t", "-subtest",
						 testdatadir,
						 dirname,
				};
				char testdirname[16];
				size_t expected_tests = multiple ? 2 : 3;
				size_t i;

				igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));
				igt_assert(create_job_list(&list, &settings));
				igt_assert(initialize_execute_state(&state, &settings, &list));

				igt_assert(execute(&state, &settings, &list));
				igt_assert_f((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0,
					     "Execute didn't create the results directory\n");

				igt_assert_f((fd = openat(dirfd, "uname.txt", O_RDONLY)) >= 0,
					     "Execute didn't create uname.txt\n");
				close(fd);
				fd = -1;

				for (i = 0; i < expected_tests; i++) {
					snprintf(testdirname, 16, "%zd", i);

					igt_assert_f((subdirfd = openat(dirfd, testdirname, O_DIRECTORY | O_RDONLY)) >= 0,
						     "Execute didn't create result directory '%s'\n", testdirname);
					assert_execution_results_exist(subdirfd);
					close(subdirfd);
				}

				snprintf(testdirname, 16, "%zd", expected_tests);
				igt_assert_f((subdirfd = openat(dirfd, testdirname, O_DIRECTORY | O_RDONLY)) < 0,
					     "Execute created too many directories\n");
			}

			igt_fixture {
				close(fd);
				close(subdirfd);
				close(dirfd);
				clear_directory(dirname);
				free_job_list(&list);
			}
		}
	}

	igt_subtest_group {
		struct job_list list;
		int dirfd = -1, subdirfd = -1, fd = -1;
		int multiple;

		igt_fixture {
			init_job_list(&list);
		}

		for (multiple = 0; multiple < 2; multiple++) {
			char dirname[] = "tmpdirXXXXXX";

			igt_fixture {
				igt_require(mkdtemp(dirname) != NULL);
				rmdir(dirname);
			}

			igt_subtest_f("execute-skipper-journal-%s", multiple ? "multiple" : "normal") {
				struct execute_state state;
				char *argv[] = { "runner",
						 multiple ? "--multiple-mode" : "--sync",
						 "-t", "skippers",
						 testdatadir,
						 dirname,
				};
				char *dump;
				char *expected_0 = multiple ?
					"skip-one\nskip-two\nexit:77 (" :
					"skip-one\nexit:77 (";
				char *expected_1 = "skip-two\nexit:77 (";

				igt_assert(parse_options(ARRAY_SIZE(argv), argv, &settings));
				igt_assert(create_job_list(&list, &settings));
				igt_assert(initialize_execute_state(&state, &settings, &list));

				igt_assert(execute(&state, &settings, &list));
				igt_assert_f((dirfd = open(dirname, O_DIRECTORY | O_RDONLY)) >= 0,
					     "Execute didn't create the results directory\n");

				igt_assert_f((fd = openat(dirfd, "uname.txt", O_RDONLY)) >= 0,
					     "Execute didn't create uname.txt\n");
				close(fd);
				fd = -1;

				igt_assert_f((subdirfd = openat(dirfd, "0", O_DIRECTORY | O_RDONLY)) >= 0,
					     "Execute didn't create result directory '0'\n");
				dump = dump_file(subdirfd, "journal.txt");
				igt_assert_f(dump != NULL,
					     "Execute didn't create the journal\n");
				/* Trim out the runtime */
				dump[strlen(expected_0)] = '\0';
				igt_assert_eqstr(dump, expected_0);
				free(dump);
				close(subdirfd);
				subdirfd = -1;

				if (!multiple) {
					igt_assert_f((subdirfd = openat(dirfd, "1", O_DIRECTORY | O_RDONLY)) >= 0,
						     "Execute didn't create result directory '1'\n");
					dump = dump_file(subdirfd, "journal.txt");
					igt_assert_f(dump != NULL,
						     "Execute didn't create the journal\n");
					dump[strlen(expected_1)] = '\0';
					igt_assert_eqstr(dump, expected_1);
					free(dump);
					close(subdirfd);
					subdirfd = -1;
				}
			}

			igt_fixture {
				close(fd);
				close(subdirfd);
				close(dirfd);
				clear_directory(dirname);
				free_job_list(&list);
			}
		}
	}

	igt_subtest("file-descriptor-leakage") {
		int i;

		/*
		 * This is a build-time test, and it's expected that
		 * all subtests are normally run. Keep this one at the
		 * end.
		 *
		 * Try to close some number of fds after stderr and
		 * expect EBADF for each one.
		 */
		for (i = 3; i < 400; i++) {
			errno = 0;
			igt_assert_neq(close(i), 0);
			igt_assert_eq(errno, EBADF);
		}
	}

	igt_fixture
		free_settings(&settings);
}
