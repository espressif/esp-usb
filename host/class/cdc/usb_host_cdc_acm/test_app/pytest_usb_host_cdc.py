# SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

from typing import Tuple

import pytest
from pytest_embedded_idf.dut import IdfDut
from pytest_embedded_idf.utils import idf_parametrize


@pytest.mark.usb_host
@pytest.mark.parametrize('count', [
    2,
], indirect=True)
@idf_parametrize('target', ['esp32s2', 'esp32s3', 'esp32p4'], indirect=['target'])
def test_usb_host_cdc(dut: Tuple[IdfDut, IdfDut]) -> None:
    device = dut[0]
    host = dut[1]

    # 1 Prepare USB device with dual interface for CDC test
    device.expect_exact('Press ENTER to see the list of tests.')
    device.write('[cdc_acm_mock_dev][dual_iface]')
    device.expect_exact('USB initialization DONE')

    ## 2 Run CDC test
    host.run_all_single_board_cases(group='cdc_acm')
    device.serial.hard_reset()


#    # 3 Prepare USB device with remote wakeup callback for test
#    device.expect_exact('Press ENTER to see the list of tests.')
#    device.write('[cdc_acm_mock_dev][early_remote_wake]')
#    device.expect_exact('USB initialization DONE')
#
#    ## 4 Run remote wakeup test
#    host.run_all_single_board_cases(group='early_remote_wake')
#    device.serial.hard_reset()


    # 5 Prepare USB device with remote wakeup callback for test
    device.expect_exact('Press ENTER to see the list of tests.')
    device.write('[cdc_acm_mock_dev][remote_wake]')
    device.expect_exact('USB initialization DONE')

    ## 6 Run remote wakeup test
    host.run_all_single_board_cases(group='remote_wake')
    device.serial.hard_reset()


    # 7 Prepare USB device with remote wakeup and sudden disconnect for test
    device.expect_exact('Press ENTER to see the list of tests.')
    device.write('[cdc_acm_mock_dev][remote_wake_dconn]')
    device.expect_exact('USB initialization DONE')

    ## 8 Run remote wakeup test with sudden disconnect
    host.run_all_single_board_cases(group='remote_wake_dconn')
    device.serial.hard_reset()
