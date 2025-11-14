# SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

from typing import Tuple

import pytest
from pytest_embedded_idf.dut import IdfDut


@pytest.mark.esp32s2
@pytest.mark.esp32s3
@pytest.mark.esp32p4
@pytest.mark.usb_host
@pytest.mark.parametrize('count', [
    2,
], indirect=True)
def test_usb_host_hid(dut: Tuple[IdfDut, IdfDut]) -> None:
    device = dut[0]
    host = dut[1]

    # Tests with mocked HID device with one Interface for HID tests
    device.expect_exact('Press ENTER to see the list of tests.')
    device.write('[hid_device]')
    device.expect_exact('HID mock device with 1xInterface (Protocol=None) has been started')

    host.run_all_single_board_cases(group='hid_host')

    # Tests with mocked HID device with two Interfaces for HID tests
    device.serial.hard_reset()
    device.expect_exact('Press ENTER to see the list of tests.')
    device.write('[hid_device2]')
    device.expect_exact('HID mock device with 2xInterfaces (Protocol=BootKeyboard, Protocol=BootMouse) has been started')

    host.run_all_single_board_cases(group='hid_host')

    # TODO: Enable during the fix https://github.com/espressif/esp-usb/pull/320
    # Expected to fail due to the overriding the freed memory during realloc in the HID Host driver

    # Tests with mocked HID device with large report descriptor (1905 bytes) for HID tests
    # device.serial.hard_reset()
    # device.expect_exact('Press ENTER to see the list of tests.')
    # device.write('[hid_device_large_report]')
    # device.expect_exact('HID mock device with large report descriptor has been started')

    # host.run_all_single_board_cases(group='hid_host')
