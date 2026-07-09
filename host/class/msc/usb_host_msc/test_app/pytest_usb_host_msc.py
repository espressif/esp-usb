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
        pytest.param(2, 'default', 'esp32h4'),
        pytest.param(2, 'default', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param(2, 'esp32p4_eco4', 'esp32p4', marks=[pytest.mark.esp32p4_eco4]),
        pytest.param(2, 'default', 'esp32s31'),
    ],
    indirect=True,
)
def test_usb_host_msc(dut: Tuple[IdfDut, IdfDut]) -> None:
    device = dut[0]
    host = dut[1]

    tests = [
        # Device test mode          Host test case group
        ("default",                 "usb_msc"),
        ("suspend_sudden_dconn",    "host_suspend_sudden_dconn"),
    ]

    for dev_test_mode, host_test_case_group in tests:
        # Prepare USB Device for test
        device.expect_exact("Press ENTER to see the list of tests.")
        # Only spiflash configuration is run in CI (no SDMMC)
        device.write(f"[usb_msc_device][spiflash][{dev_test_mode}]")
        device.expect_exact("USB initialization DONE")

        # Run tests USB Host tests
        host.run_all_single_board_cases(group=host_test_case_group)
        device.serial.hard_reset()
