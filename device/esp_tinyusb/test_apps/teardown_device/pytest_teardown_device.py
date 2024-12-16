# SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import pytest
from pytest_embedded_idf.dut import IdfDut
import serial
# import serial.tools.list_ports
from serial.tools.list_ports import comports
# import time
from time import sleep
from time import time


@pytest.mark.esp32s2
@pytest.mark.esp32s3
@pytest.mark.esp32p4
@pytest.mark.usb_device

def find_tusb_cdc(vid, pid):
    ports = comports()
    for cdc in ports:
        if cdc.vid == vid and cdc.pid == pid:
            return cdc.device
    return None

def wait_for_tusb_cdc(vid, pid, timeout=30):
    start_time = time()
    while time() - start_time < timeout:
        tusb_cdc = find_tusb_cdc(vid, pid)
        if tusb_cdc:
            return tusb_cdc
        sleep(0.5)  # Check every 0.5 seconds
    return None  

def teardown_device(amount):
    TUSB_VID = 0x303A  # Espressif TinyUSB VID
    TUSB_PID = 0x4002  # Espressif TinyUSB VID

    # Command to send and expected response
    COMMAND = "teardown"
    EXPECTED_RESPONSE = "teardown"

    # Number of iterations, must be equal to ITERATIONS in the test application
    ITERATIONS = amount

    for i in range(ITERATIONS):
        print(f"Iteration {i+1} of {ITERATIONS}")

        # Wait for the device to appear
        print("Waiting for the device to connect...")
        tusb_cdc = wait_for_tusb_cdc(TUSB_VID, TUSB_PID)
        if not tusb_cdc:
            print("Error: Device did not appear within the timeout period.")
            break

        try:
            # Open the serial port
            with serial.Serial(port=tusb_cdc, baudrate=9600, timeout=1) as cdc:
                print(f"Opened port: {tusb_cdc}")

                # Send the 'teardown' command
                cdc.write(COMMAND.encode('utf-8'))
                print(f"Sent command: {COMMAND.strip()}")

                # Wait for the response
                res = cdc.readline().decode('utf-8').strip()
                print(f"Received response: {res}")

                # Check if the response matches the expected response
                if res == EXPECTED_RESPONSE:
                    print("Response matches expected value.")
                else:
                    print("Error: Response does not match expected value.")

        except serial.SerialException as e:
            print(f"Error communicating with the serial port: {e}")
            break

        # Wait for the device to disconnect
        print("Waiting for the device to disconnect...")
        while find_tusb_cdc(TUSB_VID, TUSB_PID):
            sleep(0.5)  # Check every 0.5 seconds

    print("Finished all iterations.")

def test_usb_teardown_device(dut) -> None:
    dut.expect_exact('Press ENTER to see the list of tests.')
    dut.write('[teardown]')
    dut.expect_exact('TinyUSB: TinyUSB Driver installed')
    sleep(2)            # Some time for the OS to enumerate our USB device
    teardown_device(10) # Teardown tusb device 10 times
    
