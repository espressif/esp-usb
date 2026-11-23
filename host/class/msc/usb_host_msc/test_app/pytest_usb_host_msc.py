# SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

from typing import Tuple

import pytest
from pytest_embedded_idf.dut import IdfDut

# Create matrix of all targets <-> all configs
_PARAMS = [
    pytest.param(2, config, target, marks=[pytest.mark.eco_default] if target == 'esp32p4' else [])
    for target in ('esp32s2', 'esp32s3', 'esp32h4', 'esp32p4', 'esp32s31')
    for config in ('default', 'buffer_dma')
]
# Append additional config per-targets
_PARAMS.append(pytest.param(2, 'esp32p4_eco4', 'esp32p4', marks=[pytest.mark.esp32p4_eco4]))


@pytest.mark.usb_host
@pytest.mark.parametrize('count, config, target', _PARAMS, indirect=True)
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
