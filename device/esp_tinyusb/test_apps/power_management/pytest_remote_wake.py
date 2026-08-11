# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

from time import sleep
import pytest
from pytest_embedded_idf.dut import IdfDut

# Mainly for a local run, as there is no error when pyusb is not installed and the pytest silently fails
try:
    import usb.core
    import usb.util
except ImportError as e:
    raise RuntimeError("pyusb is not installed. Install it with: pip install pyusb") from e

# Standard USB requests (USB 2.0 spec, Table 9-4)
USB_B_REQUEST_SET_FEATURE       = 0x03

# Standard feature selectors (USB 2.0 spec, Table 9-6)
USB_FEAT_DEVICE_REMOTE_WAKEUP   = 0x01

# Bit mask belonging to the bmAttributes field of a configuration descriptor
USB_BM_ATTRIBUTES_WAKEUP        = 0x20

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
}


def set_remote_wake_on_device(VID: int, PID: int) -> None:
    '''
    Resume the device by opening it
    Set remote wakeup on device by sending SET_FEATURE ctrl transfer to the device

    :param VID: VID of the device
    :param PID: PID of the device
    '''

    # Device is currently suspended, we must open it to resume it and send the ctrl transfer
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        raise ValueError("Device not found")

    bmRequestType = usb.util.build_request_type(
                    usb.util.CTRL_OUT,
                    usb.util.CTRL_TYPE_STANDARD,
                    usb.util.CTRL_RECIPIENT_DEVICE)

    try:
        dev.ctrl_transfer(
            bmRequestType=bmRequestType,
            bRequest=USB_B_REQUEST_SET_FEATURE,
            wValue=USB_FEAT_DEVICE_REMOTE_WAKEUP,
            wIndex=0,
        )
    except usb.core.USBError as e:
        raise RuntimeError("Control transfer not sent") from e

    print("CTRL transfer sent")

    try:
        usb.util.dispose_resources(dev)
    except usb.core.USBError as e:
        raise RuntimeError("Device resources not released") from e


def check_remote_wake_feature(VID: int, PID: int, has_remote_wake: bool) -> None:
    '''
    Check if the device reports remote wakeup feature from it's configuration descriptor

    :param VID: VID of the device
    :param PID: PID of the device
    :param has_remote_wake: Expect the device to does/does not feature with remote wakeup
    '''

    sleep(2)  # Some time for the OS to enumerate our USB device
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        raise ValueError("Device not found")

    cfg = dev.get_active_configuration()
    remote_wake_supported = bool(cfg.bmAttributes & USB_BM_ATTRIBUTES_WAKEUP)

    if remote_wake_supported:
        print("Device advertises remote wakeup feature in it's descriptor")
    else:
        print("Device does not advertise remote wakeup feature in it's descriptor")

    # Assertion to fail on mismatch
    assert remote_wake_supported == has_remote_wake, (
        f"Remote wakeup capability mismatch: "
        f"expected {has_remote_wake}, "
        f"device reports {remote_wake_supported}"
    )

    try:
        usb.util.dispose_resources(dev)
    except usb.core.USBError as e:
        raise RuntimeError("Device resources not released") from e


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
def test_usb_device_remote_wakeup_en(dut: IdfDut) -> None:
    '''
    Running the test locally:
    1. Build the test app for your USB-OTG capable DUT
    2. Connect you DUT to your test runner (local machine) with USB port and flashing port
    3. Run `pytest --target <target>`

    Test procedure:
    1. Run the test on the DUT
    2. Expect one COM Port in the system
    3. Check the device's configuration descriptor, if it reports remote wakeup functionality
    4. Enable the remote wakeup by sending a ctrl transfer
    '''
    sleep(5)
    dut.expect_exact('Press ENTER to see the list of tests.')
    dut.write('[tinyusb_remote_wakeup_reporting]')
    dut.expect_exact('TinyUSB: TinyUSB Driver installed')
    sleep(2)

    # Wait for device attach event
    dut.expect_exact(TINYUSB_EVENTS['attached'])
    # Check if the device reports remote wakeup feature
    check_remote_wake_feature(DUT_VID, DUT_PID, has_remote_wake=True)

    # Expect device suspend event (auto suspend) with remote wakeup disabled
    dut.expect_exact(TINYUSB_EVENTS['suspended_remote_wake_dis'])

    # Enable remote wakeup on the device
    set_remote_wake_on_device(DUT_VID, DUT_PID)

    # Expect device to resume (ctrl transfer sent)
    dut.expect_exact(TINYUSB_EVENTS['resumed'])
    # Expect device suspend event (auto suspend) with remote wakeup enabled
    dut.expect_exact(TINYUSB_EVENTS['suspended_remote_wake_en'])

    # Device called remote wakeup

    # Expect device to resume (remote wakeup)
    dut.expect_exact(TINYUSB_EVENTS['resumed'])

    # Wait for the test app to finish
    dut.expect_exact('PM_Device_main_app: Cleanup')
    dut.expect_exact(TINYUSB_EVENTS['detached'])
    sleep(1)
