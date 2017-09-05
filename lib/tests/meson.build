lib_tests = [
	'igt_fork_helper',
	'igt_list_only',
	'igt_simulation',
	'igt_stats',
	'igt_segfault',
	'igt_subtest_group',
	'igt_assert',
	'igt_exit_handler',
	'igt_hdmi_inject',
	'igt_can_fail',
	'igt_can_fail_simple',
]

lib_fail_tests = [
	'igt_no_exit',
	'igt_no_exit_list_only',
	'igt_no_subtest',
	'igt_simple_test_subtests',
	'igt_timeout',
	'igt_invalid_subtest_name',
]

foreach lib_test : lib_tests
	exec = executable(lib_test, lib_test + '.c', install : true,
			dependencies : igt_deps)
	test('lib: ' + lib_test, exec)
endforeach

foreach lib_test : lib_fail_tests
	exec = executable(lib_test, lib_test + '.c', install : true,
			dependencies : igt_deps)
	test('lib: ' + lib_test, exec, should_fail : true)
endforeach
