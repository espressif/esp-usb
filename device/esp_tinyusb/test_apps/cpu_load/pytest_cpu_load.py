# SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import pytest
from pytest_embedded_idf.dut import IdfDut


@pytest.mark.parametrize('target', [
    'esp32s2',
    'esp32s3',
    'esp32p4',
], indirect=True)
@pytest.mark.usb_device
def test_usb_device_cpu_load(dut: IdfDut) -> None:
    dut.run_all_single_board_cases(group='ci')
