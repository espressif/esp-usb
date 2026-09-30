# SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import pytest
from pytest_embedded_idf.dut import IdfDut
import subprocess
from time import sleep, time


# Shared by every case in this app: default config on each USB-OTG target,
# plus the esp32p4 eco4 config (sdkconfig.ci.esp32p4_eco4).
_TARGET_CONFIGS = [
    pytest.param('default', 'esp32s2'),
    pytest.param('default', 'esp32s3'),
    pytest.param('default', 'esp32h4'),
    pytest.param('default', 'esp32p4', marks=[pytest.mark.eco_default]),
    pytest.param('esp32p4_eco4', 'esp32p4', marks=[pytest.mark.esp32p4_eco4]),
    pytest.param('default', 'esp32s31'),
]


class DeviceNotFoundError(Exception):
    """Custom exception for device not found within the timeout period."""
    pass


def tusb_dev_in_list(vid, pid):
    try:
        output = subprocess.check_output(["lsusb"], text=True)
        search_string = f"{vid}:{pid}"
        return search_string in output
    except Exception as e:
        print(f"Error while executing lsusb: {e}")
        raise


def wait_tusb_dev_appeared(vid, pid, timeout):
    start_time = time()
    while True:
        if tusb_dev_in_list(vid, pid):
            return True
        if time() - start_time > timeout:
            raise DeviceNotFoundError(f"Device with VID: {vid}, PID: {pid} not found within {timeout} seconds.")
        sleep(0.5)


def wait_tusb_dev_removed(vid, pid, timeout):
    start_time = time()
    while True:
        if not tusb_dev_in_list(vid, pid):
            return True
        if time() - start_time > timeout:
            raise DeviceNotFoundError(f"Device with VID: {vid}, PID: {pid} wasn't removed within {timeout} seconds.")
        sleep(0.5)


def tusb_device_teardown(iterations, timeout):
    TUSB_VID = "303a"  # Espressif TinyUSB VID
    TUSB_PID = "4002"  # Espressif TinyUSB PID

    for i in range(iterations):
        # Wait until the device is present
        print(f"Waiting for device ...")
        wait_tusb_dev_appeared(TUSB_VID, TUSB_PID, timeout)
        print("Device detected.")

        # Wait until the device is removed
        print("Waiting for the device to be removed...")
        wait_tusb_dev_removed(TUSB_VID, TUSB_PID, timeout)
        print("Device removed.")
    print("Monitoring completed.")


@pytest.mark.usb_device
@pytest.mark.parametrize(
    'config, target',
    _TARGET_CONFIGS,
    indirect=['target'],
)
def test_usb_device_runtime_config(dut: IdfDut) -> None:
    peripherals = [
        'default',
        'high_speed',
        # 'full_speed', TODO: Enable this after the P4 USB OTG 1.1 periph is connected
    ] if dut.target in ('esp32p4', 'esp32s31') else [
        'default',
        'full_speed',
    ]

    for periph in peripherals:
        dut.run_all_single_board_cases(group=periph)


# The threshold values for TinyUSB Task Run time (in cycles) for different targets
TASK_RUN_TIME_LIMITS = {
    'esp32s2': 7000,
    'esp32s3': 3000,
    'esp32p4': 1800,
    'esp32h4': 3000,
    'esp32s31': 1800,
}


def _get_run_time_th(target: str) -> int:
    assert target in TASK_RUN_TIME_LIMITS
    return TASK_RUN_TIME_LIMITS.get(target)


@pytest.mark.usb_device
@pytest.mark.parametrize(
    'config, target',
    _TARGET_CONFIGS,
    indirect=['target'],
)
def test_cpu_load_task_stat_print(dut: IdfDut) -> None:
    '''
    Test to verify that Run time and CPU load measurement for TinyUSB task is working.

    Test procedure:
    1. Run the test on the DUT
    2. Expect to see TinyUSB task CPU load printed in the output
    3. Expect TinyUSB task CPU load to be not greater than 0%
    '''
    dut.expect_exact('Press ENTER to see the list of tests.')
    dut.write('[cpu_load]')
    dut.expect_exact('Starting TinyUSB load measurement test...')
    dut.expect_exact('CPU load measurement test completed.')

    line = dut.expect(r'TinyUSB Run time: (\d+) ticks')
    run_time = int(line.group(1))
    run_time_max = _get_run_time_th(dut.target)

    assert 0 < run_time < run_time_max, f'Unexpected TinyUSB Run time: {run_time}'


@pytest.mark.usb_device
@pytest.mark.parametrize(
    'config, target',
    _TARGET_CONFIGS,
    indirect=['target'],
)
def test_usb_device_dconn_detection(dut: IdfDut) -> None:
    dut.run_all_single_board_cases(group='dconn')


@pytest.mark.usb_device
@pytest.mark.parametrize(
    'config, target',
    _TARGET_CONFIGS,
    indirect=['target'],
)
def test_usb_teardown_device(dut: IdfDut) -> None:
    dut.expect_exact('Press ENTER to see the list of tests.')
    dut.write('[teardown]')
    dut.expect_exact('TinyUSB: TinyUSB Driver installed')
    sleep(2)             # Some time for the OS to enumerate our USB device

    try:
        tusb_device_teardown(10, 10)  # Teardown tusb device: amount, timeout

    except DeviceNotFoundError as e:
        print(f"Error: {e}")
        raise

    except Exception as e:
        print(f"An unexpected error occurred: {e}")
        raise
