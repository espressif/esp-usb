# SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

from typing import Tuple

import pytest
from pytest_embedded_idf.dut import IdfDut


@pytest.mark.usb_host
@pytest.mark.parametrize(
    'count, config, target',
    [
        pytest.param(2, 'default', 'esp32s2'),
        pytest.param(2, 'default', 'esp32s3'),
        pytest.param(2, 'default', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param(2, 'esp32p4_eco4', 'esp32p4', marks=[pytest.mark.esp32p4_eco4]),
    ],
    indirect=True,
)
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

    # Tests with mocked HID device with large report descriptor (1905 bytes) for HID tests
    device.serial.hard_reset()
    device.expect_exact('Press ENTER to see the list of tests.')
    device.write('[hid_device_large_report]')
    device.expect_exact('HID mock device with large report descriptor has been started')

    host.run_all_single_board_cases(group='hid_host')

    # Tests with mocked HID device with extra large report descriptor (32KB) for HID tests
    device.serial.hard_reset()
    device.expect_exact('Press ENTER to see the list of tests.')
    device.write('[hid_device_extra_large_report]')
    device.expect_exact('HID mock device with extra large report descriptor has been started')

    host.run_all_single_board_cases(group='hid_host_extra_large_report')
