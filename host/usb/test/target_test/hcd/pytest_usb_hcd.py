# SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0

from typing import Tuple

import pytest
from pytest_embedded_idf.dut import IdfDut


@pytest.mark.usb_host_flash_disk
@pytest.mark.parametrize(
    'config, target',
    [
        pytest.param('default', 'esp32s2'),
        pytest.param('default', 'esp32s3'),
        pytest.param('default', 'esp32h4'),
        pytest.param('default', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('esp32p4_psram', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('esp32p4_eco4', 'esp32p4', marks=[pytest.mark.esp32p4_eco4]),
        pytest.param('default', 'esp32s31'),
        pytest.param('esp32s31_psram', 'esp32s31'),
    ],
    indirect=['target'],
)
def test_usb_hcd(dut: IdfDut) -> None:
    '''
        USB Host (DUT):     esp32
        USB Device:         flash disk (real usb device)
    '''
    if dut.target in ('esp32p4', 'esp32s31'):
        dut.run_all_single_board_cases(group='high_speed', reset=True)
    else:
        dut.run_all_single_board_cases(group='full_speed', reset=True)


@pytest.mark.usb_host
@pytest.mark.parametrize(
    'count, config, target',
    [
        pytest.param(2, 'default', 'esp32s2'),
        pytest.param(2, 'default', 'esp32s3'),
        pytest.param(2, 'default', 'esp32h4'),
        pytest.param(2, 'default', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param(2, 'esp32p4_eco4', 'esp32p4', marks=[pytest.mark.esp32p4_eco4]),
        pytest.param(2, 'default', 'esp32s31'),
    ],
    indirect=True,
)
def test_usb_hcd_device_mock(dut: Tuple[IdfDut, IdfDut]) -> None:
    '''
        USB Host (DUT):     esp32
        USB Device:         esp_tinyusb (mock device)
    '''
    device = dut[0]
    host = dut[1]

    tests = [
        # Device                    Host
        ("hid_dev",                 "tinyusb_mock"),
    ]

    for dev_test_mode, host_test_case_group in tests:
        # Prepare USB Device for test
        device.expect_exact("Press ENTER to see the list of tests.")
        device.write(f"[esp_tinyusb][{dev_test_mode}]")
        device.expect_exact("USB initialization DONE")

        # Run tests USB Host tests
        host.run_all_single_board_cases(group=f"intr & {host_test_case_group}")
        device.serial.hard_reset()
