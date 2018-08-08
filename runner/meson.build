jsonc = dependency('json-c', required: _runner_required)

runnerlib_sources = [ 'settings.c',
		      'job_list.c',
		      'executor.c',
		      'resultgen.c',
		    ]

runner_sources = [ 'runner.c' ]
resume_sources = [ 'resume.c' ]
results_sources = [ 'results.c' ]

if _build_runner and jsonc.found()
	subdir('testdata')

	runnerlib = static_library('igt_runner', runnerlib_sources,
				   include_directories : inc,
				   dependencies : jsonc)

	runner = executable('igt_runner', runner_sources,
			    link_with : runnerlib,
			    install : true,
			    install_dir : bindir,
			    dependencies : igt_deps)

	resume = executable('igt_resume', resume_sources,
			    link_with : runnerlib,
			    install : true,
			    install_dir : bindir,
			    dependencies : igt_deps)

	results = executable('igt_results', results_sources,
			     link_with : runnerlib,
			     install : true,
			     install_dir : bindir,
			     dependencies : igt_deps)

	build_info += 'Build test runner: Yes'
else
	build_info += 'Build test runner: No'
endif
