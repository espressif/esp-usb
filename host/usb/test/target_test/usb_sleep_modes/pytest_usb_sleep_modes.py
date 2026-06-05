# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0

import pytest
from pytest_embedded_idf.dut import IdfDut


@pytest.mark.usb_host_flash_disk
@pytest.mark.parametrize(
    'config, target',
    [
        pytest.param('default', 'esp32s2'),
        pytest.param('default', 'esp32s3'),
        pytest.param('default', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('esp32p4_eco4', 'esp32p4', marks=[pytest.mark.esp32p4_eco4]),
    ],
    indirect=['target'],
)
def test_usb_light_sleep(dut: IdfDut) -> None:
    '''Light and deep sleep test cases'''
    dut.run_all_single_board_cases(group='usb_sleep_modes')
