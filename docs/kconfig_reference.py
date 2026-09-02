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
    'esp32s31': 'IDF_TARGET_ESP32S31',
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


def _get_soc_caps_kconfig(target):
    idf_path = os.environ.get('IDF_PATH')
    if not idf_path:
        return None

    soc_caps_kconfig = (
        Path(idf_path) / 'components' / 'soc' / target / 'include' / 'soc' / 'Kconfig.soc_caps.in'
    )
    if soc_caps_kconfig.exists():
        return soc_caps_kconfig

    return None


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
        source_path = Path(source)
        if not source_path.is_absolute():
            source_path = project_path / source_path
        lines.append(f'source "{source_path.as_posix()}"\n')

    kconfig_path.write_text(''.join(lines), encoding='utf-8')


def _generate_kconfig_include(project_path, build_dir, target, output_name, kconfig_sources, extra_symbols=None,require_soc_caps=False):
    bool_symbols = _get_target_symbols(target)
    if extra_symbols:
        bool_symbols.update(extra_symbols)

    output_path = build_dir / 'inc' / output_name
    output_path.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix='esp_usb_kconfig_') as temp_dir:
        temp_dir_path = Path(temp_dir)
        kconfig_path = temp_dir_path / 'Kconfig'
        sdkconfig_path = temp_dir_path / 'sdkconfig'

        soc_caps_kconfig = _get_soc_caps_kconfig(target)
        if soc_caps_kconfig is not None:
            kconfig_sources = (soc_caps_kconfig,) + tuple(kconfig_sources)
        elif require_soc_caps:
            raise RuntimeError(
                f'Failed to generate {output_name}: set IDF_PATH to an ESP-IDF checkout '
                f'with components/soc/{target}/include/soc/Kconfig.soc_caps.in'
            )

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
        require_soc_caps=True,
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
            'SPIRAM': target in ('esp32p4', 'esp32s31'),
        },
    )


def setup(app):
    app.connect('config-inited', generate_reference)

    return {'parallel_read_safe': True, 'parallel_write_safe': True, 'version': '0.1'}
