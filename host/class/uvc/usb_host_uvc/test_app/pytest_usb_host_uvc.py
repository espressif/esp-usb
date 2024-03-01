# SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import pytest
from pytest_embedded_idf.dut import IdfDut


@pytest.mark.esp32s2
@pytest.mark.esp32s3
@pytest.mark.usb_host
def test_usb_host_uvc(dut: IdfDut) -> None:
    dut.run_all_single_board_cases(group='usb_uvc')
