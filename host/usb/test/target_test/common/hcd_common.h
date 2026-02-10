/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hcd.h"
#include "usb_private.h"
#include "usb/usb_types_ch9.h"

// ----------------------------------------------------- Macros --------------------------------------------------------

#define TEST_P4_OTG11 0 // Change this to 1 to test on OTG1.1 peripheral - only for ESP32-P4

#if TEST_P4_OTG11
#define TEST_PHY                USB_PHY_TARGET_INT
#define TEST_PORT_NUM           1
#else
#if CONFIG_IDF_TARGET_ESP32P4
#define TEST_PHY                USB_PHY_TARGET_UTMI
#else
#define TEST_PHY                USB_PHY_TARGET_INT
#endif
#define TEST_PORT_NUM           0
#endif // TEST_P4_OTG11

extern hcd_port_handle_t port_hdl;

#define URB_CONTEXT_VAL ((void *)0xDEADBEEF)

// ------------------------------------------------- HCD Event Test ----------------------------------------------------

/**
 * @brief Expect (wait) for an HCD port event
 *
 * @param port_hdl Port handle to expect event from
 * @param expected_event Port event to expect
 */
void test_hcd_expect_port_event(hcd_port_handle_t port_hdl, hcd_port_event_t expected_event);

/**
 * @brief Expect (wait) for an HCD pipe event
 *
 * @param pipe_hdl Pipe handle to expect event from
 * @param expected_event Pipe event to expect
 */
void test_hcd_expect_pipe_event(hcd_pipe_handle_t pipe_hdl, hcd_pipe_event_t expected_event);

/**
 * @brief Expect (wait) for an HCD pipe event, but none should be delivered
 *
 * This function waits for an pipe event and makes sure, that none is delivered
 *
 * @param pipe_hdl Pipe handle to expect event from
 */
void test_hcd_expect_no_pipe_event(hcd_pipe_handle_t pipe_hdl);

/**
 * @brief Get the current number of queued port events (dequeued using test_hcd_expect_port_event())
 *
 * @param port_hdl Port handle
 * @return int Number of port events currently queued
 */
int test_hcd_get_num_port_events(hcd_port_handle_t port_hdl);

/**
 * @brief Get the current number of queued pipe events (dequeued using test_hcd_expect_pipe_event())
 *
 * @param pipe_hdl Pipe handle
 * @return int Number of pipe events currently queued
 */
int test_hcd_get_num_pipe_events(hcd_pipe_handle_t pipe_hdl);

// ----------------------------------------------- Driver/Port Related -------------------------------------------------

/**
 * @brief Sets up the HCD and initializes an HCD port.
 *
 * The HCD port is left in the HCD_PORT_STATE_NOT_POWERED state
 *
 * @return hcd_port_handle_t Port handle
 */
hcd_port_handle_t test_hcd_setup(void);

/**
 * @brief Frees and HCD port and uninstalls the HCD
 *
 * @param port_hdl Port handle
 */
void test_hcd_teardown(hcd_port_handle_t port_hdl);

/**
 * @brief Wait for a connection on an HCD port
 *
 * @note HCD_PORT_CMD_POWER_ON is called internally to allow connections
 *
 * @param port_hdl Port handle
 * @return usb_speed_t Speed of the connected device
 */
usb_speed_t test_hcd_wait_for_conn(hcd_port_handle_t port_hdl);

/**
 * @brief Wait for a disconnection on an HCD port
 *
 * @note HCD_PORT_CMD_POWER_OFF is called internally to force a disconnection
 *
 * @param port_hdl Port handle
 * @param already_disabled Whether the HCD port is already in the disabled state
 */
void test_hcd_wait_for_disconn(hcd_port_handle_t port_hdl, bool already_disabled);

/**
 * @brief Suspend the root port procedure with a single HCD pipe
 *
 * This function will halt and flush a provided pipe and issue a suspend command to the root port
 *
 * @param port_hdl Port handle
 * @param pipe_hdl Pipe handle to be halted and flushed
 */
void test_hcd_root_port_suspend(hcd_port_handle_t port_hdl, hcd_pipe_handle_t pipe_hdl);

/**
 * @brief Suspend the root port procedure with multiple HCD pipes
 *
 * This function will halt and flush all pipes from a provided pipe list and issue a suspend command to the root port
 *
 * @param port_hdl Port handle
 * @param pipe_list Pipe handle list to be halted and flushed
 * @param list_len Pipe list length
 */
void test_hcd_root_port_suspend_multi_pipe(hcd_port_handle_t port_hdl, hcd_pipe_handle_t *pipe_list, int list_len);

/**
 * @brief Resume the root port procedure with a single HCD pipe
 *
 * This function will clear provided pipe and issue a resume command to the root port
 *
 * @param port_hdl Port handle
 * @param pipe_hdl Pipe handle to be cleared
 */
void test_hcd_root_port_resume(hcd_port_handle_t port_hdl, hcd_pipe_handle_t pipe_hdl);

/**
 * @brief Resume the root port procedure with multiple HCD pipes
 *
 * This function will clear all pipes from a provided pipe list and issue a resume command to the root port
 *
 * @param port_hdl Port handle
 * @param pipe_list Pipe handle list to be halted and flushed
 * @param list_len Pipe list length
 */
void test_hcd_root_port_resume_multi_pipe(hcd_port_handle_t port_hdl, hcd_pipe_handle_t *pipe_list, int list_len);

// ------------------------------------------------- Pipe alloc/free ---------------------------------------------------

/**
 * @brief Test the allocation of a pipe
 *
 * @param port_hdl Port handle
 * @param ep_desc Endpoint descriptor
 * @param dev_addr Device address of the pipe
 * @param dev_speed Device speed of the pipe
 * @return hcd_pipe_handle_t Pipe handle
 */
hcd_pipe_handle_t test_hcd_pipe_alloc(hcd_port_handle_t port_hdl, const usb_ep_desc_t *ep_desc, uint8_t dev_addr, usb_speed_t dev_speed);

/**
 * @brief Test the freeing of a pipe
 *
 * @param pipe_hdl Pipe handle
 */
void test_hcd_pipe_free(hcd_pipe_handle_t pipe_hdl);

/**
 * @brief Allocate a URB
 *
 * @param num_isoc_packets Number of isochronous packets
 * @param data_buffer_size Size of the data buffer of the URB
 * @return urb_t* URB
 */
urb_t *test_hcd_alloc_urb(int num_isoc_packets, size_t data_buffer_size);

/**
 * @brief Free a URB
 *
 * @param urb URB
 */
void test_hcd_free_urb(urb_t *urb);

// --------------------------------------------------- Enumeration -----------------------------------------------------

/**
 * @brief Do some basic enumeration of the device
 *
 * For tests that need a device to have been enumerated (such as bulk tests). This function will enumerate that device
 * using the device's default pipe. The minimal enumeration will include
 *
 * - Getting the device's descriptor and updating the default pipe's MPS
 * - Setting the device's address and updating the default pipe to use that address
 * - Setting the device to configuration 1 (i.e., the first configuration found
 *
 * @param default_pipe The connected device's default pipe
 * @return uint8_t The address of the device after enumeration
 */
uint8_t test_hcd_enum_device(hcd_pipe_handle_t default_pipe);

// ---------------------------------------------- Transfer submit ------------------------------------------------------

/**
 * @brief Ping device to check whether the device is responsive or not
 *
 * Use this function to check whether it is possible to communicate with a device.
 * For example after resuming a device, to check whether the device has been resumed correctly.
 * The function sends a get configuration descriptor request to a device, checking both, the IN and OUT transfers
 *
 * @note this function sends a control transfer to the device
 * @param default_pipe The connected device's default pipe
 * @param default_urb A default_pipe's URB used for get configuration descriptor request
 */
void test_hcd_ping_device(hcd_pipe_handle_t default_pipe, urb_t *default_urb);

#ifdef REMOTE_WAKE_HAL_SUPPORTED
/**
 * @brief Enable remote wakeup feature on device
 *
 * This function enables or disables remote wakeup on device by sending set feature or clear feature requests
 *
 * @note this function sends a control transfer to the device
 * @param default_pipe The connected device's default pipe
 * @param feature_urb A default_pipe's URB used for set feature / clear feature
 * @param enable Enable/Disable remote wake-up
 */
void test_hcd_remote_wake_enable(hcd_pipe_handle_t default_pipe, urb_t *feature_urb, bool enable);

/**
 * @brief Check if remote wakeup feature is currently enabled on device
 *
 * @note this function sends a control transfer to the device
 * @param default_pipe The connected device's default pipe
 * @param feature_urb A default_pipe's URB used for get status request
 * @return True if remote wake-up is currently enabled
 *         False if remote wake-up is currently disabled
 */
bool test_hcd_remote_wake_check(hcd_pipe_handle_t default_pipe, urb_t *get_status_urb);
#endif // REMOTE_WAKE_HAL_SUPPORTED
