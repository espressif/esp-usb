# SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

import pytest
from pytest_embedded_idf.dut import IdfDut
import serial
import serial.tools.list_ports
import time
from time import sleep


@pytest.mark.esp32s2
@pytest.mark.esp32s3
@pytest.mark.esp32p4
@pytest.mark.usb_device

def find_serial_port_by_vid_pid(vid, pid):
    """
    Find a serial port by its VID and PID.
    """
    ports = serial.tools.list_ports.comports()
    for port in ports:
        if port.vid == vid and port.pid == pid:
            return port.device
    return None

def wait_for_device(vid, pid, timeout=30):
    """
    Wait for a device with the specified VID and PID to appear.
    """
    start_time = time.time()
    while time.time() - start_time < timeout:
        serial_port = find_serial_port_by_vid_pid(vid, pid)
        if serial_port:
            return serial_port
        time.sleep(0.5)  # Check every 0.5 seconds
    return None  

def teardown():
    TARGET_VID = 0x303A  # Espressif TinyUSB VID
    TARGET_PID = 0x4002  # Espressif TinyUSB VID

    # Command to send and expected response
    COMMAND = "teardown"
    EXPECTED_RESPONSE = "teardown"

    # Number of iterations, must be equal to ITERATIONS in the test application
    ITERATIONS = 10

    for i in range(ITERATIONS):
        print(f"Iteration {i+1} of {ITERATIONS}")

        # Wait for the device to appear
        print("Waiting for the device to connect...")
        serial_port = wait_for_device(TARGET_VID, TARGET_PID)
        if not serial_port:
            print("Error: Device did not appear within the timeout period.")
            break

        try:
            # Open the serial port
            with serial.Serial(port=serial_port, baudrate=9600, timeout=1) as set:
                print(f"Opened port: {serial_port}")

                # Send the 'teardown' command
                set.write(COMMAND.encode('utf-8'))
                print(f"Sent command: {COMMAND.strip()}")

                # Wait for the response
                response = set.readline().decode('utf-8').strip()
                print(f"Received response: {response}")

                # Check if the response matches the expected response
                if response == EXPECTED_RESPONSE:
                    print("Response matches expected value.")
                else:
                    print("Error: Response does not match expected value.")

        except serial.SerialException as e:
            print(f"Error communicating with the serial port: {e}")
            break

        # Wait for the device to disconnect
        print("Waiting for the device to disconnect...")
        while find_serial_port_by_vid_pid(TARGET_VID, TARGET_PID):
            time.sleep(0.5)  # Check every 0.5 seconds

    print("Finished all iterations.")

def test_usb_teardown_device(dut) -> None:
    dut.expect_exact('Press ENTER to see the list of tests.')
    dut.write('[teardown]')
    dut.expect_exact('TinyUSB: TinyUSB Driver installed')
    sleep(2)  # Some time for the OS to enumerate our USB device
    teardown()
    
