gtk_doc = dependency('gtk-doc', required : build_docs)
if build_tests and gtk_doc.found()
	subdir('igt-gpu-tools')
elif build_docs.enabled()
	error('Documentation requires building tests')
endif

build_info += 'Build reference documentation: @0@'.format(build_docs.enabled())
