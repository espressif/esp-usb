# SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0
import pytest
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize


# No runner marker, skip this test in CI
@idf_parametrize('target', ['esp32s3'], indirect=['target'])
def test_usb_enum(dut: Dut) -> None:
    dut.run_all_single_board_cases(group='mock_enum_device', timeout=250)  # some tests take more than default timeout
