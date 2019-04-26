jsonc = dependency('json-c', required: _runner_required)

runnerlib_sources = [ 'settings.c',
		      'job_list.c',
		      'executor.c',
		      'resultgen.c',
		    ]

runner_sources = [ 'runner.c' ]
resume_sources = [ 'resume.c' ]
results_sources = [ 'results.c' ]
runner_test_sources = [ 'runner_tests.c' ]
runner_json_test_sources = [ 'runner_json_tests.c' ]

if not _build_tests and _runner_required
	error('Building test runner requires building tests')
endif

if _build_runner and _build_tests and jsonc.found()
	subdir('testdata')

	runnerlib = static_library('igt_runner', runnerlib_sources,
				   include_directories : inc,
				   dependencies : jsonc)

	runner = executable('igt_runner', runner_sources,
			    link_with : runnerlib,
			    install : true,
			    install_dir : bindir,
			    install_rpath : bindir_rpathdir,
			    dependencies : igt_deps)

	resume = executable('igt_resume', resume_sources,
			    link_with : runnerlib,
			    install : true,
			    install_dir : bindir,
			    install_rpath : bindir_rpathdir,
			    dependencies : igt_deps)

	results = executable('igt_results', results_sources,
			     link_with : runnerlib,
			     install : true,
			     install_dir : bindir,
			     install_rpath : bindir_rpathdir,
			     dependencies : igt_deps)

	runner_test = executable('runner_test', runner_test_sources,
				 c_args : '-DTESTDATA_DIRECTORY="@0@"'.format(testdata_dir),
				 link_with : runnerlib,
				 install : false,
				 dependencies : igt_deps)
	test('runner', runner_test)

	runner_json_test = executable('runner_json_test', runner_json_test_sources,
				      c_args : '-DJSON_TESTS_DIRECTORY="@0@"'.format(join_paths(meson.current_source_dir(), 'json_tests_data')),
				      link_with : runnerlib,
				      install : false,
				      dependencies : [igt_deps, jsonc])
	test('runner_json', runner_json_test)

	build_info += 'Build test runner: Yes'
else
	build_info += 'Build test runner: No'
endif
