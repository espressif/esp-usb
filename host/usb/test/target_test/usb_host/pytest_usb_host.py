# SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0

import pytest
from pytest_embedded_idf.dut import IdfDut

@pytest.mark.esp32s2
@pytest.mark.esp32s3
@pytest.mark.esp32p4
@pytest.mark.usb_host_flash_disk

# Skip the idf parametrize for now, keep this commented
#@idf_parametrize('target', ['esp32s2', 'esp32s3', 'esp32p4'], indirect=['target'])

def test_usb_host(dut: IdfDut) -> None:
    if dut.target == 'esp32p4':
        dut.run_all_single_board_cases(group='high_speed', reset=True)
    else:
        dut.run_all_single_board_cases(group='full_speed', reset=True)
