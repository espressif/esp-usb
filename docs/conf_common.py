# SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""
Common configuration for ESP-USB documentation
"""

import esp_docs.conf_docs as esp_conf_docs
from esp_docs.conf_docs import *  # noqa: F403,F401


extensions += [  # Needed as a trigger for running doxygen
               'esp_docs.esp_extensions.dummy_build_system',
               'esp_docs.esp_extensions.run_doxygen',
               'sphinx_reredirects'
               ]
# Languages supported
languages = ['en']

# Project targets (used as URL slugs)
idf_targets = ['esp32s2', 'esp32s3', 'esp32p4', 'esp32h4']

# Default redirects map; will be updated in setup once config values are available.
redirects = {}

def _build_redirects(release_url, idf_target=None):
    result = {}
    targets = [idf_target] if idf_target else idf_targets
    for target in targets:
        result[f'../../../zh_CN/{target}/html/index.html'] = f'../../../en/{release_url}/{target}/index.html'
        result[f'../../../zh_CN/{target}/html/introduction.html'] = f'../../../en/{release_url}/{target}/introduction.html'
        result[f'../../../zh_CN/{target}/html/usb_device.html'] = f'../../../en/{release_url}/{target}/usb_device.html'
        result[f'../../../zh_CN/{target}/html/usb_host.html'] = f'../../../en/{release_url}/{target}/usb_host.html'
    return result


def _configure_redirects(app, config):
    release_url = config.release
    idf_target_val = config.idf_target
    config.redirects = _build_redirects(release_url, idf_target=idf_target_val)


def setup(app):
    app.connect('config-inited', _configure_redirects)
    return esp_conf_docs.setup(app)

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
