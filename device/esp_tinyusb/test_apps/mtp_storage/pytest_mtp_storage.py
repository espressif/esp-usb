# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import hashlib
import os
import re
import subprocess
import time
from collections.abc import Iterator
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from urllib.parse import quote

import pytest
from pytest_embedded_idf.dut import IdfDut

_RUN_EXTENDED_MTP_TESTS = os.environ.get('MTP_RUN_EXTENDED_TESTS') == '1'
_EXTENDED_MTP_TEST_REASON = 'set MTP_RUN_EXTENDED_TESTS=1 to run extended MTP host tests'


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
def test_usb_device_mtp_storage(dut: IdfDut, config: str) -> None:
    dut.run_all_single_board_cases(group=['ci'])


def _gio(*args: str, check: bool = True, input_data: bytes | None = None, timeout: float = 10) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(['gio', *args], check=check, input=input_data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)


def _mtp_uri(bus: str, device: str, *parts: str) -> str:
    path = '/'.join(quote(part, safe='') for part in parts)
    return f'mtp://[usb:{bus},{device}]/{path}'


def _wait_for_mtp_uri() -> str:
    usb_id = os.environ.get('MTP_TEST_USB_ID', '303a:4040').lower()
    deadline = time.monotonic() + 30
    last_error = ''
    while time.monotonic() < deadline:
        result = subprocess.run(['lsusb', '-d', usb_id], check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        devices = result.stdout.splitlines()
        if result.returncode != 0 and result.stderr:
            last_error = result.stderr.strip()
        if len(devices) > 1:
            raise AssertionError(f'multiple MTP test devices match {usb_id}: {devices}')
        if len(devices) == 1:
            match = re.match(r'Bus (\d{3}) Device (\d{3}):', devices[0])
            if match is None:
                raise AssertionError(f'cannot parse lsusb output: {devices[0]}')
            uri = _mtp_uri(*match.groups())
            try:
                mount = _gio('mount', uri, check=False, timeout=min(5, max(0.1, deadline - time.monotonic())))
                listing = _gio('list', uri, check=False, timeout=min(5, max(0.1, deadline - time.monotonic())))
            except subprocess.TimeoutExpired:
                last_error = 'gio mount timed out'
                continue
            if listing.returncode == 0:
                return uri
            last_error = (mount.stderr + listing.stderr).decode(errors='replace').strip()
        time.sleep(0.5)
    raise AssertionError(f'MTP test device {usb_id} was not mounted within 30 seconds: {last_error or "not enumerated"}')


@pytest.fixture
def mtp_host_uri(dut: IdfDut) -> Iterator[str]:
    dut.expect_exact('Press ENTER to see the list of tests.')
    dut.write('[mtp][manual]')
    dut.expect_exact('MTP_HOST_READY', timeout=20)

    uri = ''
    try:
        uri = _wait_for_mtp_uri()
        yield uri
    finally:
        if uri:
            _gio('mount', '-u', uri, check=False)
        dut.serial.hard_reset()


def _test_workspace(uri: str, storage: str, name: str) -> str:
    return uri + quote(storage, safe='') + '/' + quote(name, safe='')


def _remove_if_present(uri: str) -> None:
    if _gio('info', uri, check=False).returncode == 0:
        _gio('remove', uri)


def _read_positive_int_env(name: str, default: int) -> int:
    value = int(os.environ.get(name, default))
    if value <= 0:
        raise ValueError(f'{name} must be greater than zero')
    return value


def _read_nonnegative_float_env(name: str) -> float | None:
    raw = os.environ.get(name)
    if raw is None:
        return None
    value = float(raw)
    if value < 0:
        raise ValueError(f'{name} must not be negative')
    return value


def _assert_payload(actual: bytes, expected: bytes, context: str) -> None:
    if actual == expected:
        return
    mismatch = next((index for index, (actual_byte, expected_byte) in enumerate(zip(actual, expected)) if actual_byte != expected_byte), min(len(actual), len(expected)))
    pytest.fail(f'{context}: payload mismatch at offset {mismatch}, expected {len(expected)} bytes with SHA-256 {hashlib.sha256(expected).hexdigest()}, got {len(actual)} bytes with SHA-256 {hashlib.sha256(actual).hexdigest()}')


@pytest.mark.usb_device
@pytest.mark.parametrize(
    'config, target',
    [
        pytest.param('default', 'esp32s2'),
        pytest.param('default', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('esp32p4_eco4', 'esp32p4', marks=[pytest.mark.esp32p4_eco4]),
    ],
    indirect=['target'],
)
def test_usb_device_mtp_manual_pc_access(mtp_host_uri: str, tmp_path: Path, config: str) -> None:
    payload = bytes(range(256)) * 17
    source = tmp_path / 'ci_upload.bin'
    source.write_bytes(payload)
    empty_source = tmp_path / 'ci_empty.bin'
    empty_source.write_bytes(b'')
    edited = b'Edited by Linux GVfs through MTP.\n'
    test_name = f'ci_host_{os.getpid()}'
    for storage in ('Flash FATFS', 'SDCard FATFS', 'NAND FATFS'):
        root = mtp_host_uri + quote(storage, safe='') + '/'
        probe = root + quote('host_access_probe.txt', safe='')
        nested = root + quote('folder_from_device', safe='') + '/' + quote('nested.txt', safe='')
        deleted = root + quote('delete_me.txt', safe='')
        workspace = root + quote(f'{test_name}_workspace', safe='')
        subdir = workspace + '/' + quote('ci_subdir', safe='')
        archive = root + quote(f'{test_name}_archive', safe='')
        uploaded = subdir + '/' + quote('ci_upload.bin', safe='')
        renamed = subdir + '/' + quote('ci_renamed.bin', safe='')
        child = subdir + '/' + quote('ci_child.bin', safe='')
        moved = archive + '/' + quote('ci_renamed.bin', safe='')
        moved_dir = archive + '/' + quote('ci_subdir', safe='')
        moved_child = moved_dir + '/' + quote('ci_child.bin', safe='')
        empty = workspace + '/' + quote('ci_empty.bin', safe='')

        assert _gio('cat', probe).stdout == b'This file is pre-created by the MTP manual PC access test.\n'
        assert _gio('cat', nested).stdout == b'This nested file verifies folder traversal from the PC.\n'
        _gio('mkdir', workspace, subdir, archive)
        _gio('copy', source.as_uri(), uploaded)
        _gio('rename', uploaded, 'ci_renamed.bin')
        _gio('move', renamed, archive)
        _gio('save', moved, input_data=edited)
        _gio('copy', source.as_uri(), child)
        _gio('move', subdir, moved_dir)
        assert _gio('cat', moved_child).stdout == payload
        _gio('copy', empty_source.as_uri(), empty)
        assert _gio('cat', empty).stdout == b''
        _gio('remove', deleted)

        downloaded = tmp_path / f'{storage}.bin'
        _gio('copy', moved, downloaded.as_uri())
        assert downloaded.read_bytes() == edited
        assert _gio('info', deleted, check=False).returncode != 0
        _gio('remove', moved)
        _gio('remove', moved_child)
        _gio('remove', empty)
        _gio('remove', moved_dir)
        _gio('remove', workspace)
        archive_entries = _gio('list', archive).stdout
        assert archive_entries == b'', archive_entries.decode(errors='replace')
        _gio('remove', archive)


@pytest.mark.usb_device
@pytest.mark.skipif(not _RUN_EXTENDED_MTP_TESTS, reason=_EXTENDED_MTP_TEST_REASON)
@pytest.mark.timeout(180)
@pytest.mark.parametrize(
    'config, target',
    [
        pytest.param('default', 'esp32s2'),
        pytest.param('default', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('esp32p4_eco4', 'esp32p4', marks=[pytest.mark.esp32p4_eco4]),
    ],
    indirect=['target'],
)
def test_usb_device_mtp_host_performance(mtp_host_uri: str, tmp_path: Path, config: str) -> None:
    file_size = _read_positive_int_env('MTP_PERF_FILE_SIZE', 256 * 1024)
    iterations = _read_positive_int_env('MTP_PERF_ITERATIONS', 3)
    min_upload_mbps = _read_nonnegative_float_env('MTP_MIN_UPLOAD_MBPS')
    min_download_mbps = _read_nonnegative_float_env('MTP_MIN_DOWNLOAD_MBPS')
    payload = (bytes(range(256)) * ((file_size + 255) // 256))[:file_size]
    expected_hash = hashlib.sha256(payload).digest()
    source = tmp_path / 'mtp_perf_source.bin'
    source.write_bytes(payload)

    for storage in ('Flash FATFS', 'SDCard FATFS', 'NAND FATFS'):
        workspace = _test_workspace(mtp_host_uri, storage, f'ci_perf_{os.getpid()}')
        _gio('mkdir', workspace)
        upload_rates = []
        download_rates = []
        try:
            for iteration in range(iterations + 1):
                remote = workspace + '/' + quote(f'payload_{iteration}.bin', safe='')
                downloaded = tmp_path / f'{storage}_{iteration}.bin'
                start = time.monotonic()
                _gio('copy', source.as_uri(), remote, timeout=30)
                upload_seconds = time.monotonic() - start
                start = time.monotonic()
                _gio('copy', remote, downloaded.as_uri(), timeout=30)
                download_seconds = time.monotonic() - start
                downloaded_data = downloaded.read_bytes()
                if hashlib.sha256(downloaded_data).digest() != expected_hash:
                    _assert_payload(downloaded_data, payload, f'{storage} iteration {iteration}')
                _gio('remove', remote)
                if iteration > 0:
                    upload_rates.append(file_size / upload_seconds / (1024 * 1024))
                    download_rates.append(file_size / download_seconds / (1024 * 1024))
            upload_mbps = sum(upload_rates) / len(upload_rates)
            download_mbps = sum(download_rates) / len(download_rates)
            print(f'MTP performance [{storage}]: upload={upload_mbps:.3f} MiB/s download={download_mbps:.3f} MiB/s size={file_size} bytes iterations={iterations}')
            if min_upload_mbps is not None:
                assert upload_mbps >= min_upload_mbps, f'{storage} upload {upload_mbps:.3f} MiB/s is below {min_upload_mbps:.3f} MiB/s'
            if min_download_mbps is not None:
                assert download_mbps >= min_download_mbps, f'{storage} download {download_mbps:.3f} MiB/s is below {min_download_mbps:.3f} MiB/s'
        finally:
            for iteration in range(iterations + 1):
                _remove_if_present(workspace + '/' + quote(f'payload_{iteration}.bin', safe=''))
            _remove_if_present(workspace)


@pytest.mark.usb_device
@pytest.mark.skipif(not _RUN_EXTENDED_MTP_TESTS, reason=_EXTENDED_MTP_TEST_REASON)
@pytest.mark.timeout(180)
@pytest.mark.parametrize(
    'config, target',
    [
        pytest.param('default', 'esp32s2'),
        pytest.param('default', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('esp32p4_eco4', 'esp32p4', marks=[pytest.mark.esp32p4_eco4]),
    ],
    indirect=['target'],
)
def test_usb_device_mtp_host_concurrency(mtp_host_uri: str, tmp_path: Path, config: str) -> None:
    worker_count = _read_positive_int_env('MTP_CONCURRENCY_WORKERS', 4)
    operations_per_worker = _read_positive_int_env('MTP_CONCURRENCY_OPERATIONS', 8)
    file_size = _read_positive_int_env('MTP_CONCURRENCY_FILE_SIZE', 16 * 1024)
    storage = 'Flash FATFS'
    workspace = _test_workspace(mtp_host_uri, storage, f'ci_concurrent_{os.getpid()}')
    source_paths = {}
    expected_payloads = {}
    for worker in range(worker_count):
        for operation in range(operations_per_worker):
            key = (worker, operation)
            seed = f'worker={worker},operation={operation}\n'.encode()
            payload = (seed * ((file_size + len(seed) - 1) // len(seed)))[:file_size]
            source = tmp_path / f'mtp_concurrent_{worker}_{operation}.bin'
            source.write_bytes(payload)
            source_paths[key] = source
            expected_payloads[key] = payload
    _gio('mkdir', workspace)

    def run_worker(worker: int) -> None:
        for operation in range(operations_per_worker):
            name = f'worker_{worker}_{operation}.bin'
            renamed_name = f'worker_{worker}_{operation}_renamed.bin'
            remote = workspace + '/' + quote(name, safe='')
            renamed = workspace + '/' + quote(renamed_name, safe='')
            key = (worker, operation)
            _gio('copy', source_paths[key].as_uri(), remote, timeout=30)
            _assert_payload(_gio('cat', remote, timeout=30).stdout, expected_payloads[key], f'worker {worker} operation {operation} before rename')
            _gio('rename', remote, renamed_name, timeout=30)
            _assert_payload(_gio('cat', renamed, timeout=30).stdout, expected_payloads[key], f'worker {worker} operation {operation} after rename')
            _gio('remove', renamed, timeout=30)

    try:
        with ThreadPoolExecutor(max_workers=worker_count) as executor:
            futures = [executor.submit(run_worker, worker) for worker in range(worker_count)]
            while not all(future.done() for future in futures):
                assert _gio('list', workspace, check=False, timeout=30).returncode == 0
                time.sleep(0.05)
            for future in futures:
                future.result()
        assert _gio('list', workspace).stdout == b''
    finally:
        for worker in range(worker_count):
            for operation in range(operations_per_worker):
                _remove_if_present(workspace + '/' + quote(f'worker_{worker}_{operation}.bin', safe=''))
                _remove_if_present(workspace + '/' + quote(f'worker_{worker}_{operation}_renamed.bin', safe=''))
        _remove_if_present(workspace)


@pytest.mark.usb_device
@pytest.mark.skipif(not _RUN_EXTENDED_MTP_TESTS, reason=_EXTENDED_MTP_TEST_REASON)
@pytest.mark.timeout(180)
@pytest.mark.parametrize(
    'config, target',
    [
        pytest.param('default', 'esp32s2'),
        pytest.param('default', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('esp32p4_eco4', 'esp32p4', marks=[pytest.mark.esp32p4_eco4]),
    ],
    indirect=['target'],
)
def test_usb_device_mtp_host_many_files(mtp_host_uri: str, tmp_path: Path, config: str) -> None:
    file_count = _read_positive_int_env('MTP_MANY_FILES_COUNT', 16)
    assert file_count <= 16, 'MTP_MANY_FILES_COUNT must not exceed the test app cache budget of 16'
    storage = 'Flash FATFS'
    workspace = _test_workspace(mtp_host_uri, storage, f'ci_many_{os.getpid()}')
    source_paths = []
    payloads = (b'', b'x', bytes(range(64)), bytes(range(256)) * 4)
    for index, payload in enumerate(payloads):
        source = tmp_path / f'mtp_many_source_{index}.bin'
        source.write_bytes(payload)
        source_paths.append(source)

    names = []
    refill_names = []
    for index in range(file_count):
        suffix = ('短文件', 'medium_name', 'long_' + 'a' * 72)[index % 3]
        names.append(f'{index:03d}_{suffix}.bin')
        refill_names.append(f'refill_{index:03d}_{suffix}.bin')

    _gio('mkdir', workspace)
    try:
        for index, name in enumerate(names):
            _gio('copy', source_paths[index % len(source_paths)].as_uri(), workspace + '/' + quote(name, safe=''), timeout=30)

        listed_names = set(_gio('list', workspace, timeout=30).stdout.decode().splitlines())
        assert listed_names == set(names)
        for name in names:
            assert _gio('info', workspace + '/' + quote(name, safe=''), check=False, timeout=30).returncode == 0
        for index in range(0, file_count, max(1, file_count // 12)):
            data = _gio('cat', workspace + '/' + quote(names[index], safe=''), timeout=30).stdout
            assert data == payloads[index % len(payloads)]

        removed_indexes = list(range(0, file_count, 2))
        for index in removed_indexes:
            _gio('remove', workspace + '/' + quote(names[index], safe=''), timeout=30)
        for index in removed_indexes:
            _gio('copy', source_paths[index % len(source_paths)].as_uri(), workspace + '/' + quote(refill_names[index], safe=''), timeout=30)

        final_names = set(_gio('list', workspace, timeout=30).stdout.decode().splitlines())
        expected_names = {name for index, name in enumerate(names) if index % 2 != 0} | {refill_names[index] for index in removed_indexes}
        assert final_names == expected_names
        assert not any('.mtp_tmp' in name or '.mtp_bak' in name for name in final_names)
    finally:
        for name in names + refill_names:
            _remove_if_present(workspace + '/' + quote(name, safe=''))
        _remove_if_present(workspace)
