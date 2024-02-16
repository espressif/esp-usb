# SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

from typing import Tuple

import pytest
from pytest_embedded_idf.dut import IdfDut


@pytest.mark.esp32s2
@pytest.mark.esp32s3
@pytest.mark.usb_host
@pytest.mark.parametrize('count', [
    2,
], indirect=True)
def test_usb_host_hid(dut: Tuple[IdfDut, IdfDut]) -> None:
    device = dut[0]
    host = dut[1]

    # 3.1 Prepare USB device with one Interface for HID tests
    device.expect_exact('Press ENTER to see the list of tests.')
    device.write('[hid_device]')
    device.expect_exact('HID mock device with 1xInterface (Protocol=None) has been started')

    # 3.2 Run HID tests
    host.run_all_single_board_cases(group='hid_host')

    # 3.3 Prepare USB device with two Interfaces for HID tests
    device.serial.hard_reset()
    device.expect_exact('Press ENTER to see the list of tests.')
    device.write('[hid_device2]')
    device.expect_exact('HID mock device with 2xInterfaces (Protocol=BootKeyboard, Protocol=BootMouse) has been started')

    # 3.4 Run HID tests
    host.run_all_single_board_cases(group='hid_host')
