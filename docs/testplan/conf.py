# -*- coding: utf-8 -*-
# SPDX-License-Identifier: (GPL-2.0 OR MIT)

import sys
import os
from shutil import which

extensions = []

if which('rst2pdf'):
    extensions.append('rst2pdf.pdfbuilder')

source_suffix = '.rst'
master_doc = 'index'

project = 'IGT Test Tools'
copyright = 'Intel'
author = 'IGT developers'
language = 'en'

exclude_patterns = []
todo_include_todos = False

html_theme = "nature"

html_css_files = []
html_static_path = ['.']
html_copy_source = False

html_use_smartypants = False
html_sidebars = { '**': ['searchbox.html', 'localtoc.html']}
htmlhelp_basename = 'IGT'

html_theme_options = {
    "body_max_width": "1520px",
    "sidebarwidth": "400px",
}

# rst2pdf
pdf_documents = [
    ('index', u'xe_tests', u'IGT Xe Tests', u'IGT authors'),
]
