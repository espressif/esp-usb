# SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

from typing import Tuple

import pytest
from pytest_embedded_idf.dut import IdfDut


@pytest.mark.esp32s2
@pytest.mark.esp32s3
@pytest.mark.esp32p4
@pytest.mark.usb_host
@pytest.mark.parametrize('count', [
    2,
], indirect=True)
def test_usb_host_msc(dut: Tuple[IdfDut, IdfDut]) -> None:
    device = dut[0]
    host = dut[1]

    # 1 Prepare USB device for MSC test
    device.expect_exact('Press ENTER to see the list of tests.')
    device.write('[usb_msc_device]')
    device.expect_exact('USB initialization DONE')

    # 2 Run MSC test
    host.run_all_single_board_cases(group='usb_msc')
