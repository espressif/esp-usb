# SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0

import pytest
from pytest_embedded_idf.dut import IdfDut


@pytest.mark.usb_host_flash_disk
@pytest.mark.parametrize(
    'config, target',
    [
        pytest.param('default', 'esp32s2'),
        pytest.param('buffer_dma', 'esp32s2'),
        pytest.param('default', 'esp32s3'),
        pytest.param('buffer_dma', 'esp32s3'),
        pytest.param('default', 'esp32h4'),
        pytest.param('buffer_dma', 'esp32h4'),
        pytest.param('default', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('buffer_dma', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('esp32p4_psram', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('esp32p4_eco4', 'esp32p4', marks=[pytest.mark.esp32p4_eco4]),
        pytest.param('default', 'esp32s31'),
        pytest.param('buffer_dma', 'esp32s31'),
    ],
    indirect=['target'],
)
def test_usb_hcd(config: str, dut: IdfDut) -> None:
    '''Run HCD tests'''

    # Get speed group based on target
    speed_group = 'high_speed' if dut.target in ('esp32p4', 'esp32s31') else 'full_speed'

    # Run target tests for DMA config
    if config == 'buffer_dma':

        transfer_type_group=[]
        transfer_type_group.append('bulk')
        transfer_type_group.append('ctrl')

        for transfer_type in transfer_type_group:
            dut.run_all_single_board_cases(group=f'{speed_group}&{transfer_type}', reset=True)

    # Run target tests for any other config
    else:
        dut.run_all_single_board_cases(group=speed_group, reset=True)
