# SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""
Common configuration for ESP-USB documentation
"""

from esp_docs.conf_docs import *  # noqa: F403,F401


extensions += [  # Needed as a trigger for running doxygen
               'esp_docs.esp_extensions.dummy_build_system',
               'esp_docs.esp_extensions.run_doxygen',
               ]
# Languages supported
languages = ['en', 'zh_CN']

# Project targets (used as URL slugs)
idf_targets = ['esp32s2', 'esp32s3', 'esp32p4', 'esp32h4']

# GitHub repository information
github_repo = 'espressif/esp-usb'
project_homepage = 'https://github.com/espressif/esp-usb'

# Initialize html_context if not already defined
html_context = {}
html_context['github_user'] = 'espressif'
html_context['github_repo'] = 'esp-usb'

# Static files path
html_static_path = ['../_static']

# Project slug for URL
project_slug = 'esp-usb'

# Versions URL (relative to HTML root)
versions_url = '_static/docs_version.js'

# PDF file prefix
pdf_file_prefix = u'esp-usb'
