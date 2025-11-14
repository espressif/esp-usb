# SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import pytest
from pytest_embedded_idf.dut import IdfDut

@pytest.mark.parametrize('target', [
    'esp32s2',
    'esp32s3',
    'esp32p4',
], indirect=True)
@pytest.mark.usb_device
def test_usb_device_runtime_config(dut: IdfDut) -> None:
    peripherals = [
        'default',
        'high_speed',
        # 'full_speed', TODO: Enable this after the P4 USB OTG 1.1 periph is connected
    ] if dut.target == 'esp32p4' else [
        'default',
        'full_speed',
    ]

    for periph in peripherals:
        dut.run_all_single_board_cases(group=periph)
