# SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import pytest
from pytest_embedded_idf.dut import IdfDut
import serial
from serial.tools.list_ports import comports
from time import sleep, time

def get_tusb_cdc_device(vid, pid):
    ports = comports()
    for cdc in ports:
        if cdc.vid == vid and cdc.pid == pid:
            return cdc.device
    return None

def wait_for_tusb_cdc(vid, pid, timeout=30):
    start_time = time()
    while time() - start_time < timeout:
        sleep(0.5)  # Check every 0.5 seconds
        tusb_cdc = get_tusb_cdc_device(vid, pid)
        if tusb_cdc:
            return tusb_cdc
    return None  

def teardown_device(key_len, amount):
    TUSB_VID = 0x303A  # Espressif TinyUSB VID
    TUSB_PID = 0x4002  # Espressif TinyUSB VID

    # Command to send and expected response
    COMMAND =  b'\xAA' * key_len
    EXPECTED_RESPONSE =  b'\x55' * key_len

    # Number of iterations, must be equal to ITERATIONS in the test application
    ITERATIONS = amount

    for i in range(ITERATIONS):
        print(f"Iteration {i+1} of {ITERATIONS}")

        # Wait for the device to appear
        print("Waiting for the device to connect...")
        tusb_cdc = wait_for_tusb_cdc(TUSB_VID, TUSB_PID)
        if not tusb_cdc:
            print("Error: Device did not appear within the timeout period.")
            assert True

        try:
            # Open the serial port
            with serial.Serial(port=tusb_cdc, baudrate=9600, timeout=1) as cdc:
                print(f"Opened port: {tusb_cdc}")
                # Send the key command
                res = cdc.write(COMMAND) 
                assert res == key_len
                # Get the response
                res = cdc.readline()
                assert len(res) == key_len
                # Explicitly close the cdc
                cdc.close()
                # Check if the response matches the expected response
                if res == EXPECTED_RESPONSE:
                    print("Response matches expected value.")
                else:
                    print(f"Sent {len(COMMAND)}: {COMMAND.hex().upper()}")
                    print(f"Received {len(res)}: {res.hex().upper()}")
                    raise Exception("Error: Response does not match expected value.")

        except serial.SerialException as e:
            print(f"Error communicating with the serial port: {e}")
            raise

        # Wait for the device to disconnect
        print("Waiting for the device to disconnect...")
        while get_tusb_cdc_device(TUSB_VID, TUSB_PID):
            sleep(0.1)  # Poll every 0.1 second while tinyusb cdc dev still in list

    print("Finished all iterations.")

@pytest.mark.esp32s2
@pytest.mark.esp32s3
@pytest.mark.esp32p4
@pytest.mark.usb_device
def test_usb_teardown_device(dut) -> None:
    dut.expect_exact('Press ENTER to see the list of tests.')
    dut.write('[teardown]')
    dut.expect_exact('TinyUSB: TinyUSB Driver installed')
    sleep(2)            # Some time for the OS to enumerate our USB device
    if dut.target == 'esp32p4':
        MPS = 512
    else:
        MPS = 64
    # On Linux, the serial port kept opened, while len==MPS, https://github.com/pyserial/pyserial/issues/753
    # that can be seen via Beagle: CDC OUT transaction appears only when terminal is closed.
    teardown_device(MPS, 4) # Teardown tusb device
    
