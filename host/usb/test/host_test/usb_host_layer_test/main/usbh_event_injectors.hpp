/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file usbh_event_injectors.hpp
 *
 * USBH event injectors for USB Host layer Linux host tests.
 *
 * Each injector sets the CMock expectations the host layer triggers while
 * handling the event, injects the event through the registered USBH mock
 * callback and asserts ESP_OK. Requires an installed USB Host library with
 * MOCK_CB_USBH (see usb_host_layer_test_fixture.hpp).
 */

#pragma once

#include <stdint.h>
#include <catch2/catch_test_macros.hpp>
#include "usb_host.h"
#include "usb_private.h"
#include "mock_usb_host_layer_test_helpers.h"

extern "C" {
#include "Mockhub.h"
}

/** USBH event injectors: set the required CMock expectations and inject the event, asserting ESP_OK. */
namespace inject_usbh_event {

/**
 * @brief Inject USBH_EVENT_CTRL_XFER: completed control transfer
 *
 * @param dev_hdl Handle of the device the transfer was submitted to
 * @param urb URB of the completed transfer; its callback is routed to urb->usb_host_client
 */
inline void ctrl_xfer(usb_device_handle_t dev_hdl, urb_t *urb)
{
    REQUIRE(ESP_OK == usbh_mock_event_ctrl_xfer(dev_hdl, urb));
}

/**
 * @brief Inject USBH_EVENT_NEW_DEV: new device on the root port
 *
 * @param dev_addr Address assigned to the new device
 */
inline void new_dev(uint8_t dev_addr)
{
    // Return NOT_SUPPORTED, otherwise there is no new device event (device is treated as an ext. hub)
    hub_dev_new_ExpectAndReturn(dev_addr, ESP_ERR_NOT_SUPPORTED);
    REQUIRE(ESP_OK == usbh_mock_event_new_dev(dev_addr));
}

/**
 * @brief Inject USBH_EVENT_DEV_GONE: device unexpectedly disconnected
 *
 * @param dev_addr Address of the gone device
 * @param dev_hdl Handle of the gone device, as held by its owner client
 */
inline void dev_gone(uint8_t dev_addr, usb_device_handle_t dev_hdl)
{
    // Return NOT_SUPPORTED, otherwise there is no device gone event (device is treated as an ext. hub)
    hub_dev_gone_ExpectAndReturn(dev_addr, ESP_ERR_NOT_SUPPORTED);
    REQUIRE(ESP_OK == usbh_mock_event_dev_gone(dev_addr, dev_hdl));
}

/**
 * @brief Inject USBH_EVENT_DEV_FREE: device object freed, its hub node is recycled
 *
 * @param dev_uid Unique ID of the freed device
 * @param parent_dev_hdl Handle of the parent device
 * @param parent_port_num Port on the parent the device was attached to
 */
inline void dev_free(unsigned dev_uid, usb_device_handle_t parent_dev_hdl, uint8_t parent_port_num)
{
    hub_node_recycle_ExpectAndReturn(dev_uid, ESP_OK);
    REQUIRE(ESP_OK == usbh_mock_event_dev_free(dev_uid, parent_dev_hdl, parent_port_num));
}

/**
 * @brief Inject USBH_EVENT_ALL_FREE: all devices freed, sets the ALL_FREE library event flag
 */
inline void all_free(void)
{
    REQUIRE(ESP_OK == usbh_mock_event_all_free());
}

/**
 * @brief Inject USBH_EVENT_DEV_REMOVED: device removed after all clients closed it
 *
 * @param dev_addr Address of the removed device
 */
inline void dev_removed(uint8_t dev_addr)
{
    REQUIRE(ESP_OK == usbh_mock_event_dev_removed(dev_addr));
}

/**
 * @brief Inject USBH_EVENT_DEV_SUSPEND: device was suspended
 *
 * @param dev_addr Address of the suspended device
 * @param dev_hdl Handle of the suspended device, as held by its owner client
 */
inline void dev_suspend(uint8_t dev_addr, usb_device_handle_t dev_hdl)
{
    REQUIRE(ESP_OK == usbh_mock_event_dev_suspend(dev_addr, dev_hdl));
}

/**
 * @brief Inject USBH_EVENT_DEV_RESUME: suspended device resumed
 *
 * @param dev_addr Address of the resumed device
 * @param dev_hdl Handle of the resumed device, as held by its owner client
 */
inline void dev_resume(uint8_t dev_addr, usb_device_handle_t dev_hdl)
{
    REQUIRE(ESP_OK == usbh_mock_event_dev_resume(dev_addr, dev_hdl));
}

/**
 * @brief Inject USBH_EVENT_ALL_IDLE: all devices idle, root hub is marked ready to suspend
 */
inline void all_idle(void)
{
    hub_root_mark_suspend_ExpectAndReturn(ESP_OK);
    REQUIRE(ESP_OK == usbh_mock_event_all_idle());
}
} // namespace inject_usbh_event
