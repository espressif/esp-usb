# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

from time import sleep
import pytest
from serial import Serial, SerialException
from serial.tools.list_ports import comports
from pytest_embedded_idf.dut import IdfDut

# Device Under Test VID:PID
DUT_VID = 0x303A
DUT_PID = 0x4002

# Tinyusb device events from device event handler
TINYUSB_EVENTS = {
    "attached":                         "TINYUSB_EVENT_ATTACHED",
    "detached":                         "TINYUSB_EVENT_DETACHED",
    "resumed":                          "TINYUSB_EVENT_RESUMED",
    "suspended_remote_wake_dis":        "TINYUSB_EVENT_SUSPENDED_REMOTE_WAKE_DIS",
    "suspended_remote_wake_en":         "TINYUSB_EVENT_SUSPENDED_REMOTE_WAKE_EN",
    "light_sleep_enter":                "LIGHT_SLEEP_ENTER",
    "light_sleep_data_rx":              "LIGHT_SLEEP_DATA_RX",
    "user_resumed":                     "USER_EVENT_RESUMED",
    "user_suspended_remote_wake_dis":   "USER_EVENT_SUSPENDED_REMOTE_WAKE_DIS",
    "user_suspended_remote_wake_en":    "USER_EVENT_SUSPENDED_REMOTE_WAKE_EN",
}

def find_dut_cdc_ports() -> list[str]:
    '''Return serial port paths for the TinyUSB CDC device.'''
    ports = []
    for p in comports():
        if (p.vid == DUT_VID and p.pid == DUT_PID):
            ports.append(p.device)
    return ports

def run_suspend_resume_cdc_test(
    dut: IdfDut,
    *,
    resumed_event: str,
    suspended_event: str,
) -> None:
    '''Run the write/read cycle test body'''
    dut.expect_exact('TinyUSB: TinyUSB Driver installed')
    sleep(2)  # Some time for the OS to enumerate our USB device

    # Find device with Espressif TinyUSB VID/PID
    ports = find_dut_cdc_ports()

    if len(ports) == 0:
        raise Exception('TinyUSB COM port not found')

    try:
        with Serial(ports[0], timeout=2) as cdc:
            dut.expect_exact(TINYUSB_EVENTS['attached'])

            # Wait for auto suspend (set to 3 seconds)
            # This expect_exact is ignored by pytest when running second rerun of flaky test (unknown reason),
            # making the test fail in further steps, adding explicit sleep(3) to wait for the suspend events
            sleep(3)
            dut.expect_exact(suspended_event)
            sleep(2)

            for i in range(5):
                print(f"Power cycle iteration {i}.")

                # Resume the device by accessing it
                cdc.write(b'Time to resume\r\n')
                res = cdc.readline()
                assert b'Time to suspend\r\n' in res

                dut.expect_exact(resumed_event)

                # Wait for auto suspend (set to 3 seconds)
                dut.expect_exact(suspended_event)

                # Stay suspended for a while
                sleep(2)

    except SerialException as e:
        raise RuntimeError(f"Failed to open CDC device on {ports[0]}") from e

    # Wait for the test app to finish
    dut.expect_exact('PM_Device_main_app: Cleanup')
    dut.expect_exact(TINYUSB_EVENTS['detached'])
    sleep(1)

def run_pm_light_sleep_suspend_resume_test(
    dut: IdfDut,
    *,
    resumed_event: str,
    suspended_event: str,
) -> None:
    '''Run PM-only suspend/resume test without host CDC resume.

    On FS targets the host cannot wake the SoC from light sleep. The DUT wakes itself
    with a sleep timer, signals USB remote wakeup, and pytest only observes events.
    '''
    dut.expect_exact('TinyUSB: TinyUSB Driver installed')
    sleep(2)  # Some time for the OS to enumerate our USB device

    ports = find_dut_cdc_ports()
    if len(ports) == 0:
        raise Exception('TinyUSB COM port not found')

    try:
        # Open the CDC port so the host enables remote wakeup, but do not write to it
        with Serial(ports[0], timeout=2):
            dut.expect_exact(TINYUSB_EVENTS['attached'])

            for i in range(5):
                print(f"PM light sleep iteration {i}.")

                # Auto-suspend
                dut.expect_exact(suspended_event)

                # Wait for the timer to wake the SoC from light sleep and call the remote wakeup
                dut.expect_exact(resumed_event)

    except SerialException as e:
        raise RuntimeError(f"Failed to open CDC device on {ports[0]}") from e

    dut.expect_exact('PM_Device_main_app: Cleanup')
    dut.expect_exact(TINYUSB_EVENTS['detached'])
    sleep(1)

@pytest.mark.usb_device
@pytest.mark.flaky(reruns=2, reruns_delay=10)
@pytest.mark.parametrize(
    'config, target',
    [
        pytest.param('default', 'esp32s2'),
        pytest.param('default', 'esp32s3'),
        pytest.param('default', 'esp32h4'),
        pytest.param('default', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('esp32p4_eco4', 'esp32p4', marks=[pytest.mark.esp32p4_eco4]),
        pytest.param('default', 'esp32s31'),
        pytest.param('pm', 'esp32s2'),
        pytest.param('pm', 'esp32s3'),
        pytest.param('pm', 'esp32h4'),
        pytest.param('pm', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('pm', 'esp32s31'),
        # Only HS targets for otg wake
        pytest.param('pm_otg_wake', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('pm_otg_wake', 'esp32s31'),
        pytest.param('pm_otg_wake_no_cb', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('pm_otg_wake_no_cb', 'esp32s31'),
    ],
    indirect=['target'],
)
def test_usb_device_suspend_resume_signaling(config: str, dut: IdfDut) -> None:
    '''
    Running the test locally:
    1. Build the test app for your USB-OTG capable DUT
    2. Connect you DUT to your test runner (local machine) with USB port and flashing port
    3. Run `pytest --target <target>`

    Test procedure:
    1. Run the test on the DUT
    2. Expect one COM Port in the system
    3. Open it and and test power management of the USB device (Suspend/Resume)
    4. Suspend: Device enters suspended state after some time of inactivity
    5. Resume: Device is resumed by accessing it (sending some data to it)
    '''
    sleep(5) # When rerunning flaky test, to re-initialize the device
    dut.expect_exact('Press ENTER to see the list of tests.')

    # Check whether the suspend and resume callbacks are registered inside the esp_tinyusb
    user_cb = not (dut.app.sdkconfig.get('TINYUSB_SUSPEND_CALLBACK', True) and dut.app.sdkconfig.get('TINYUSB_RESUME_CALLBACK', True))

    resumed_event=TINYUSB_EVENTS['resumed']
    suspended_event=TINYUSB_EVENTS['suspended_remote_wake_en']

    if user_cb:
        resumed_event=TINYUSB_EVENTS['user_resumed']
        suspended_event=TINYUSB_EVENTS['user_suspended_remote_wake_en']

    if config == 'default' or config == 'esp32p4_eco4':
        dut.write('[tinyusb_suspend_resume_events]')
    elif config == 'pm':
        dut.write('[tinyusb_pm]')
    elif config == 'pm_otg_wake':
        dut.write('[tinyusb_pm_otg_wake]')
    elif config == 'pm_otg_wake_no_cb':
        dut.write('[tinyusb_pm_otg_wake_user_events]')

    if config == 'pm':
        run_pm_light_sleep_suspend_resume_test(
            dut,
            resumed_event=resumed_event,
            suspended_event=suspended_event,
        )
    else:
        run_suspend_resume_cdc_test(
            dut,
            resumed_event=resumed_event,
            suspended_event=suspended_event,
        )

@pytest.mark.usb_device
@pytest.mark.flaky(reruns=2, reruns_delay=10)
@pytest.mark.parametrize(
    'config, target',
    [
        pytest.param('default', 'esp32s2'),
        pytest.param('default', 'esp32s3'),
        pytest.param('default', 'esp32h4'),
        pytest.param('default', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('esp32p4_eco4', 'esp32p4', marks=[pytest.mark.esp32p4_eco4]),
        pytest.param('default', 'esp32s31'),
    ],
    indirect=['target'],
)
def test_usb_cdc_device_pm_public_api(dut: IdfDut) -> None:
    '''
    Running the test locally:
    1. Build the test app for your USB-OTG capable DUT
    2. Connect you DUT to your test runner (local machine) with USB port and flashing port
    3. Run `pytest --target <target>`

    Test procedure:
    1. Run the test on the DUT
    2. Expect one COM Port in the system
    3. Open it and wait for the device to enter suspended state
    4. Wait for the DUT to complete public API error handling checks while suspended
    5. Resume the device by sending data to it
    6. Verify the device sends data that was queued during suspended state
    7. Wait for the device to enter suspended state again
    '''
    sleep(5) # When rerunning flaky test, to re-initialize the device
    dut.expect_exact('Press ENTER to see the list of tests.')
    dut.write('[cdc_device_pm_public_api]')
    dut.expect_exact('TinyUSB: TinyUSB Driver installed')
    sleep(2)  # Some time for the OS to enumerate our USB device

    # Find device with Espressif TinyUSB VID/PID
    ports = []
    for p in comports():
        if (p.vid == DUT_VID and p.pid == DUT_PID):
            ports.append(p.device)

    if len(ports) == 0:
        raise Exception('TinyUSB COM port not found')

    try:
        with Serial(ports[0], timeout=2) as cdc:
            dut.expect_exact(TINYUSB_EVENTS['attached'])

            # Wait for auto suspend (set to 3 seconds)
            # This expect_exact is ignored by pytest when running second rerun of flaky test (unknown reason),
            # making the test fail in further steps, adding explicit sleep(3) to wait for the suspend events
            sleep(3)
            dut.expect_exact(TINYUSB_EVENTS['suspended_remote_wake_en'])

            # Some time for the DUT to finish error handling test
            sleep(1)
            # Resume the device by accessing it and expect resume event
            cdc.write(b'Time to resume\r\n')
            dut.expect_exact(TINYUSB_EVENTS['resumed'])

            # Expect response from the device, which was queued during suspended state and sent after resuming
            res = cdc.readline()
            assert b'Hello from suspended state\r\n' in res

            # Wait for auto suspend (set to 3 seconds)
            dut.expect_exact(TINYUSB_EVENTS['suspended_remote_wake_en'])

    except SerialException as e:
        raise RuntimeError(f"Failed to open CDC device on {ports[0]}") from e

    # Wait for the test app to finish
    dut.expect_exact('PM_Device_main_app: Cleanup')
    sleep(1)

@pytest.mark.usb_device
@pytest.mark.flaky(reruns=2, reruns_delay=10)
@pytest.mark.parametrize(
    'config, target',
    [
        # Only HS port targets
        pytest.param('otg_wake', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('otg_wake', 'esp32s31'),
    ],
    indirect=['target'],
)
def test_usb_device_light_sleep_usb_wakeup(dut: IdfDut) -> None:
    '''
    Running the test locally:
    1. Build the test app for your USB-OTG capable DUT
    2. Connect you DUT to your test runner (local machine) with USB port and flashing port
    3. Run `pytest --target <target>`

    Test procedure:
    1. Run the light-sleep wakeup Unity test on the DUT
    2. Wait for USB attach and auto suspend
    3. After the DUT enters light sleep, access the CDC port to wake the SoC
    4. Send data to the device and verify the echoed reply
    5. Expect USB wakeup, bus resume, and data reception markers from the DUT
    '''
    sleep(5)
    dut.expect_exact('Press ENTER to see the list of tests.')
    dut.write('[tinyusb_light_sleep_otg_wake]')
    dut.expect_exact('TinyUSB: TinyUSB Driver installed')
    sleep(2)

    ports = find_dut_cdc_ports()
    if len(ports) == 0:
        raise Exception('TinyUSB COM port not found')

    try:
        with Serial(ports[0], timeout=2) as cdc:

            dut.expect_exact(TINYUSB_EVENTS['attached'])
            dut.expect_exact(TINYUSB_EVENTS['suspended_remote_wake_en'])
            dut.expect_exact(TINYUSB_EVENTS['light_sleep_enter'])
            # Stay in light sleep for some time
            sleep(3)
            cdc.write(b'Light sleep wake\r\n')
            res = cdc.readline()
            assert b'Light sleep ok\r\n' in res
    except SerialException as e:
        raise RuntimeError(f"Failed to open CDC device on {ports[0]}") from e

    dut.expect_exact(TINYUSB_EVENTS['resumed'])
    dut.expect_exact(TINYUSB_EVENTS['light_sleep_data_rx'])
    # Wait for the test app to finish
    dut.expect_exact('PM_Device_main_app: Cleanup')
    dut.expect_exact(TINYUSB_EVENTS['detached'])
    sleep(1)

@pytest.mark.usb_device
@pytest.mark.flaky(reruns=2, reruns_delay=10)
@pytest.mark.parametrize(
    'config, target',
    [
        pytest.param('default', 'esp32s2'),
        pytest.param('default', 'esp32s3'),
        pytest.param('default', 'esp32h4'),
        pytest.param('default', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('esp32p4_eco4', 'esp32p4', marks=[pytest.mark.esp32p4_eco4]),
        pytest.param('default', 'esp32s31'),
        pytest.param('pm', 'esp32s2'),
        pytest.param('pm', 'esp32s3'),
        pytest.param('pm', 'esp32h4'),
        pytest.param('pm', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('pm', 'esp32s31'),
        pytest.param('pm_otg_wake_no_cb', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('pm_otg_wake_no_cb', 'esp32s31'),
        pytest.param('pm_otg_wake', 'esp32p4', marks=[pytest.mark.eco_default]),
        pytest.param('pm_otg_wake', 'esp32s31'),
    ],
    indirect=['target'],
)
def test_usb_pm_public_api(config: str, dut: IdfDut) -> None:
    '''Testing tinyusb public API with basic configs'''

    if config == 'default' or config == 'esp32p4_eco4':
        dut.run_all_single_board_cases(group='driver_init_deinit')
    elif config == 'pm_otg_wake_no_cb':
        dut.run_all_single_board_cases(group='public_api_pm_otg_wake_no_cb')
    else:
        dut.run_all_single_board_cases(group='public_api_tinyusb_pm')
