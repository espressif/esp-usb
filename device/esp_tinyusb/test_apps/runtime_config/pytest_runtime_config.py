# SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import pytest
from pytest_embedded_idf.dut import IdfDut

@pytest.mark.esp32s2
@pytest.mark.esp32s3
@pytest.mark.esp32p4
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

@pytest.mark.esp32s2
@pytest.mark.esp32s3
@pytest.mark.esp32p4
@pytest.mark.usb_device
def test_cpu_load_task_stat_print(dut: IdfDut) -> None:
    '''
    Test to verify that Run time and CPU load measurement for TinyUSB task is working.
    This test runs only on runtime_config test app.

    Test procedure:
    1. Run the test on the DUT
    2. Expect to see TinyUSB task CPU load printed in the output
    3. Expect TinyUSB task CPU load to be not greater than 0%
    '''
    dut.expect_exact('Press ENTER to see the list of tests.')
    dut.write('[cpu_load]')
    dut.expect_exact('Starting TinyUSB load measurement test...')
    dut.expect_exact('CPU load measurement test completed.')

    line = dut.expect(r'TinyUSB Run time: (\d+) ticks')
    run_time = int(line.group(1))
    assert 0 < run_time < 3000, f'Unexpected TinyUSB Run time: {run_time}'
