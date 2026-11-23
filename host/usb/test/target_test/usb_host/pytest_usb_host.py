# SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0

import pytest
from pytest_embedded_idf.dut import IdfDut

# Create matrix of all targets <-> all configs
_PARAMS = [
    pytest.param(config, target, marks=[pytest.mark.eco_default] if target == 'esp32p4' else [])
    for target in ('esp32s2', 'esp32s3', 'esp32h4', 'esp32p4', 'esp32s31')
    for config in ('default', 'buffer_dma')
]
# Append additional config per-targets
_PARAMS.append(pytest.param('esp32p4_eco4', 'esp32p4', marks=[pytest.mark.esp32p4_eco4]))


@pytest.mark.usb_host_flash_disk
@pytest.mark.parametrize('config, target', _PARAMS, indirect=['target'])
def test_usb_host(dut: IdfDut) -> None:
    '''Run USB Host tests'''

    if dut.target in ('esp32p4', 'esp32s31'):
        dut.run_all_single_board_cases(group='high_speed', reset=True)
    else:
        dut.run_all_single_board_cases(group='full_speed', reset=True)
