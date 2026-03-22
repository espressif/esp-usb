# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

"""Generate ESP-USB specific Kconfig reference include files for Sphinx."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path


_TARGET_SYMBOLS = {
    'esp32s2': 'IDF_TARGET_ESP32S2',
    'esp32s3': 'IDF_TARGET_ESP32S3',
    'esp32p4': 'IDF_TARGET_ESP32P4',
    'esp32h4': 'IDF_TARGET_ESP32H4',
}

_DEVICE_KCONFIG_SOURCES = (
    Path('device/esp_tinyusb/Kconfig'),
)

_HOST_KCONFIG_SOURCES = (
    Path('host/usb/Kconfig'),
)


def _get_target_symbols(target):
    if target not in _TARGET_SYMBOLS:
        raise ValueError(f'Unsupported docs target: {target}')

    enabled_symbol = _TARGET_SYMBOLS[target]
    return {symbol: symbol == enabled_symbol for symbol in _TARGET_SYMBOLS.values()}


def _write_top_level_kconfig(kconfig_path, project_path, kconfig_sources, bool_symbols):
    lines = [
        'mainmenu "ESP-USB"\n',
        '\n',
    ]

    for symbol, enabled in bool_symbols.items():
        default_value = 'y' if enabled else 'n'
        lines.extend([
            f'config {symbol}\n',
            '    bool\n',
            f'    default {default_value}\n',
            '\n',
        ])

    for source in kconfig_sources:
        source_path = (project_path / source).as_posix()
        lines.append(f'source "{source_path}"\n')

    kconfig_path.write_text(''.join(lines), encoding='utf-8')


def _generate_kconfig_include(project_path, build_dir, target, output_name, kconfig_sources, extra_symbols=None):
    bool_symbols = _get_target_symbols(target)
    if extra_symbols:
        bool_symbols.update(extra_symbols)

    output_path = build_dir / 'inc' / output_name
    output_path.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix='esp_usb_kconfig_') as temp_dir:
        temp_dir_path = Path(temp_dir)
        kconfig_path = temp_dir_path / 'Kconfig'
        sdkconfig_path = temp_dir_path / 'sdkconfig'

        _write_top_level_kconfig(kconfig_path, project_path, kconfig_sources, bool_symbols)
        sdkconfig_path.write_text('', encoding='utf-8')

        env = os.environ.copy()
        env['IDF_TARGET'] = target

        result = subprocess.run(
            [
                sys.executable,
                '-m',
                'kconfgen',
                '--kconfig',
                str(kconfig_path),
                '--config',
                str(sdkconfig_path),
                '--output',
                'docs',
                str(output_path),
            ],
            cwd=project_path,
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )

        if result.returncode != 0:
            error_output = result.stderr.strip() or result.stdout.strip() or 'kconfgen failed'
            raise RuntimeError(f'Failed to generate {output_name}: {error_output}')


def generate_reference(app, _config):
    build_dir = Path(os.path.dirname(app.doctreedir.rstrip(os.sep)))
    project_path = Path(app.config.project_path)
    target = app.config.idf_target

    _generate_kconfig_include(
        project_path,
        build_dir,
        target,
        'usb_device_kconfig.inc',
        _DEVICE_KCONFIG_SOURCES,
    )

    _generate_kconfig_include(
        project_path,
        build_dir,
        target,
        'usb_host_kconfig.inc',
        _HOST_KCONFIG_SOURCES,
        extra_symbols={
            'SOC_USB_OTG_SUPPORTED': True,
            'IDF_EXPERIMENTAL_FEATURES': False,
            'SPIRAM': target == 'esp32p4',
        },
    )


def setup(app):
    app.connect('config-inited', generate_reference)

    return {'parallel_read_safe': True, 'parallel_write_safe': True, 'version': '0.1'}
