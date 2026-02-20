# SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import pytest
from pytest_embedded_idf.dut import IdfDut
from pytest_embedded_idf.utils import idf_parametrize


#@pytest.mark.usb_device
#@idf_parametrize('target', ['esp32s2', 'esp32s3', 'esp32p4'], indirect=['target'])
@pytest.mark.parametrize(
    'config, target',
    [
        pytest.param('default', 'esp32s2', marks=[pytest.mark.usb_device]),
        pytest.param('default', 'esp32s3', marks=[pytest.mark.usb_device]),
        pytest.param('default', 'esp32p4', marks=[pytest.mark.usb_device, pytest.mark.eco_default]),
        pytest.param('esp32p4_eco4', 'esp32p4', marks=[pytest.mark.usb_device, pytest.mark.esp32p4_eco4]),
    ],
    indirect=['target'],
)
def test_usb_device_msc_storage(dut: IdfDut) -> None:
    dut.run_all_single_board_cases(group=['ci']) # Run all test cases in the 'ci' group
