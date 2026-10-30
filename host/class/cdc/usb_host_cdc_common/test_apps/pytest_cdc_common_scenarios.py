# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

from pathlib import Path
from typing import Tuple

import pytest
from pytest_embedded_idf.dut import IdfDut

COMMON_APP = Path(__file__).resolve().parent
DEVICE_APP = COMMON_APP.parent.parent / 'usb_host_cdc_acm' / 'test_app'


@pytest.mark.usb_host
@pytest.mark.parametrize(
    'count,target,app_path',
    [(2, 'esp32s3', f'{DEVICE_APP}|{COMMON_APP}')],
    indirect=True,
    ids=['esp32s3-dual-cdc'],
)
def test_cdc_common_usage_scenarios(dut: Tuple[IdfDut, IdfDut]) -> None:
    device, host = dut
    device.expect_exact('Press ENTER to see the list of tests.')
    device.write('[cdc_acm_mock_dev][dual_iface]')
    device.expect_exact('USB initialization DONE')

    scenarios = [case for case in host.test_menu if 'cdc_scenarios' in case.groups]
    assert len(scenarios) >= 15, f'Expected at least 15 usage scenarios, found {len(scenarios)}'
    # Reset isolates failures so subsequent cases can still report independent results.
    host.run_all_single_board_cases(group='cdc_scenarios', reset=True, timeout=90)
