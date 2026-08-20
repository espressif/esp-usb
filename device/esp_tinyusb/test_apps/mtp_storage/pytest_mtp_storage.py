# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import os
import re
import subprocess
import time
from pathlib import Path
from urllib.parse import quote

import pytest
from pytest_embedded_idf.dut import IdfDut


@pytest.mark.usb_device
@pytest.mark.parametrize(
    'config, target',
    [
        pytest.param('default', 'esp32s2'),
        pytest.param('default', 'esp32s3'),
        pytest.param('default', 'esp32h4'),
        pytest.param('default', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('esp32p4_eco4', 'esp32p4', marks=[pytest.mark.esp32p4_eco4]),
        pytest.param('default', 'esp32s31'),
    ],
    indirect=['target'],
)
def test_usb_device_mtp_storage(dut: IdfDut) -> None:
    dut.run_all_single_board_cases(group=['ci'])


def _gio(*args: str, check: bool = True, input_data: bytes | None = None) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(['gio', *args], check=check, input=input_data, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def _mtp_uri(bus: str, device: str, *parts: str) -> str:
    path = '/'.join(quote(part, safe='') for part in parts)
    return f'mtp://[usb:{bus},{device}]/{path}'


def _wait_for_mtp_uri() -> str:
    usb_id = os.environ.get('MTP_TEST_USB_ID', '303a:4040').lower()
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        result = subprocess.run(['lsusb', '-d', usb_id], check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        devices = result.stdout.splitlines()
        if len(devices) > 1:
            raise AssertionError(f'multiple MTP test devices match {usb_id}: {devices}')
        if len(devices) == 1:
            match = re.match(r'Bus (\d{3}) Device (\d{3}):', devices[0])
            if match is None:
                raise AssertionError(f'cannot parse lsusb output: {devices[0]}')
            uri = _mtp_uri(*match.groups())
            _gio('mount', uri, check=False)
            if _gio('list', uri, check=False).returncode == 0:
                return uri
        time.sleep(0.5)
    raise AssertionError(f'MTP test device {usb_id} was not mounted within 30 seconds')


@pytest.mark.usb_device_mtp
@pytest.mark.parametrize('config, target', [pytest.param('default', 'esp32s3')], indirect=['target'])
def test_usb_device_mtp_manual_pc_access(dut: IdfDut, tmp_path: Path) -> None:
    dut.expect_exact('Press ENTER to see the list of tests.')
    dut.write('[mtp][manual]')
    dut.expect_exact('MTP_HOST_READY', timeout=20)

    uri = ''
    try:
        uri = _wait_for_mtp_uri()
        payload = bytes(range(256)) * 17
        source = tmp_path / 'ci_upload.bin'
        source.write_bytes(payload)
        edited = b'Edited by Linux GVfs through MTP.\n'
        for storage in ('Flash FATFS', 'SDCard FATFS', 'NAND FATFS'):
            root = uri + quote(storage, safe='') + '/'
            probe = root + quote('host_access_probe.txt', safe='')
            nested = root + quote('folder_from_device', safe='') + '/' + quote('nested.txt', safe='')
            edit = root + quote('edit_me.txt', safe='')
            deleted = root + quote('delete_me.txt', safe='')
            uploaded = root + quote('ci_upload.bin', safe='')
            renamed = root + quote('ci_renamed.bin', safe='')

            assert _gio('cat', probe).stdout == b'This file is pre-created by the MTP manual PC access test.\n'
            assert _gio('cat', nested).stdout == b'This nested file verifies folder traversal from the PC.\n'
            _gio('remove', '-f', uploaded, renamed)
            _gio('copy', source.as_uri(), uploaded)
            _gio('move', uploaded, renamed)
            _gio('save', edit, input_data=edited)
            _gio('remove', deleted)

            downloaded = tmp_path / f'{storage}.bin'
            _gio('copy', renamed, downloaded.as_uri())
            assert downloaded.read_bytes() == payload
            assert _gio('cat', edit).stdout == edited
            assert _gio('info', deleted, check=False).returncode != 0
    finally:
        if uri:
            _gio('mount', '-u', uri, check=False)
        dut.serial.hard_reset()
