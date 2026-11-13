/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <catch2/catch_test_macros.hpp>
#include "usb/cdc_acm_host.h"
#include "mock_add_usb_device.h"
#include "common_test_fixtures.hpp"
#include "cdc_host_descriptor_parsing.h"
#include "usb_helpers.h"

extern "C" {
#include "Mockusb_host.h"
}

static std::mutex transfer_mutex;
static std::vector<usb_transfer_t *> pending_transfers;
static std::vector<usb_transfer_t *> completed_transfers;
static usb_host_client_handle_t mock_client;
static usb_transfer_t *timed_out_control;
static SemaphoreHandle_t control_completed;
static mock_usb_transfer_response_t tx_response;

static esp_err_t register_client(const usb_host_client_config_t *config, usb_host_client_handle_t *client, int calls)
{
    esp_err_t ret = usb_host_client_register_mock_callback(config, client, calls);
    mock_client = *client;
    return ret;
}

static esp_err_t handle_events(usb_host_client_handle_t client, TickType_t wait, int calls)
{
    esp_err_t ret = usb_host_client_handle_events_mock_callback(client, wait, calls);
    std::vector<usb_transfer_t *> completed;
    {
        std::lock_guard<std::mutex> lock(transfer_mutex);
        completed.swap(completed_transfers);
    }
    for (auto *transfer : completed) {
        transfer->status = USB_TRANSFER_STATUS_CANCELED;
        transfer->actual_num_bytes = 0;
        transfer->callback(transfer);
        if (transfer == timed_out_control) {
            xSemaphoreGive(control_completed);
        }
    }
    return ret;
}

static esp_err_t submit_transfer(usb_transfer_t *transfer, int)
{
    if ((transfer->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) || tx_response == MOCK_USB_TRANSFER_TIMEOUT) {
        std::lock_guard<std::mutex> lock(transfer_mutex);
        pending_transfers.push_back(transfer);
        return ESP_OK;
    }
    transfer->status = tx_response == MOCK_USB_TRANSFER_SUCCESS ? USB_TRANSFER_STATUS_COMPLETED : USB_TRANSFER_STATUS_ERROR;
    transfer->actual_num_bytes = tx_response == MOCK_USB_TRANSFER_SUCCESS ? transfer->num_bytes : 0;
    transfer->callback(transfer);
    return ESP_OK;
}

static esp_err_t flush_endpoint(usb_device_handle_t device, uint8_t endpoint, int)
{
    {
        std::lock_guard<std::mutex> lock(transfer_mutex);
        for (auto it = pending_transfers.begin(); it != pending_transfers.end();) {
            if ((*it)->device_handle == device && (*it)->bEndpointAddress == endpoint) {
                completed_transfers.push_back(*it);
                it = pending_transfers.erase(it);
            } else {
                ++it;
            }
        }
    }
    return usb_host_client_unblock_mock_callback(mock_client, 0);
}

esp_err_t test_cdc_control_timeout(usb_host_client_handle_t, usb_transfer_t *transfer, int)
{
    timed_out_control = transfer;
    return ESP_OK;
}

void test_cdc_complete_control_timeout(void)
{
    // Deliver the late USB completion before reusing the timed-out control transfer.
    control_completed = xSemaphoreCreateBinary();
    REQUIRE(control_completed != nullptr);
    {
        std::lock_guard<std::mutex> lock(transfer_mutex);
        completed_transfers.push_back(timed_out_control);
    }
    usb_host_client_unblock_mock_callback(mock_client, 0);
    REQUIRE(xSemaphoreTake(control_completed, pdMS_TO_TICKS(1000)) == pdTRUE);
    vSemaphoreDelete(control_completed);
    control_completed = nullptr;
    timed_out_control = nullptr;
}

/**
 * @brief CMock expectations for current CDC device
 *
 * Only used for mocked usb host expectations
 *
 * For example, based on the descriptors, we record that a certain device has CTRL EP, IN EP, OUT EP and NO notif EP
 * Then, during the device opening we expect usb_host_transfer_alloc() exactly 3 times (CTRL, IN, OUT)
 * Also, during the device closing we expect usb_host_transfer_free() exactly 3 times (CTRL, IN, OUT)
 * Different device could have CTRL, IN, OUT and NOTIF, thus we must expect those mocked functions exactly 4 times
 */
typedef struct {
    unsigned reset_count;
    struct {
        bool has_out;
        bool has_in;
        uint8_t in_bEndpointAddress;
        uint8_t out_bEndpointAddress;
    } data;
    struct {
        bool has_xfer;
        uint8_t bEndpointAddress;
        bool has_separate_interface;
    } notif;
} cdc_dev_expects_t;

/** Per-device CMock state keyed by CDC handle (supports multiple open devices). */
static std::unordered_map<cdc_acm_dev_hdl_t, cdc_dev_expects_t *> s_cdc_dev_expects_by_hdl;

/**
 * @brief Lookup table with CMock expectations for current device
 *
 * The function goes through the lookup table to find the CMock expectations for the current device
 * based on the device handle (CDC handle) and returns a pointer to the expectations of that device
 *
 * @param dev_hdl Device handle
 * @return pointer to cmock expectations for this device, or nullptr if not found
 */
static cdc_dev_expects_t *_test_cdc_expects_lookup(cdc_acm_dev_hdl_t dev_hdl)
{
    auto it = s_cdc_dev_expects_by_hdl.find(dev_hdl);
    return (it != s_cdc_dev_expects_by_hdl.end()) ? it->second : nullptr;
}

/**
 * @brief Free all CMock expectations and clear the lookup table
 */
static void _test_cdc_expects_free_all(void)
{
    for (auto &kv : s_cdc_dev_expects_by_hdl) {
        free(kv.second);
    }
    s_cdc_dev_expects_by_hdl.clear();
}

/**
 * @brief Free CMock expectations for a specific device and remove it from the lookup table
 *
 * The function goes through the lookup table to find the CMock expectations for the current device based on the device handle (CDC handle).
 *
 * @param dev_hdl Device handle to be freed
 */
static void _test_cdc_expects_free(cdc_acm_dev_hdl_t dev_hdl)
{
    auto dev_hdl_found = s_cdc_dev_expects_by_hdl.find(dev_hdl);
    if (dev_hdl_found != s_cdc_dev_expects_by_hdl.end()) {
        free(dev_hdl_found->second);
        s_cdc_dev_expects_by_hdl.erase(dev_hdl_found);
    }
}

/**
 * @brief Create CMock expectations for current device
 *
 * This function creates CMock expectations for a USB device,
 *
 * @param[in] dev_address Device address
 * @param[in] interface_index Interface index to be used
 * @param[in] dev_config CDC-ACM Host device config struct
 * @param[out] out_expects Allocated expectations for this open (caller registers on success)
 *
 * @return
 *   - ESP_OK: Mock expectations created successfully
 *   - ESP_ERR_NO_MEM: Not enough memory
 */
static esp_err_t _test_create_cmock_expectations(uint8_t dev_address, uint8_t interface_index, const cdc_acm_host_device_config_t *dev_config,
                                                 cdc_dev_expects_t **out_expects)
{
    cdc_dev_expects_t *cdc_dev_expects = (cdc_dev_expects_t *)calloc(1, sizeof(cdc_dev_expects_t));
    if (cdc_dev_expects == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    *out_expects = cdc_dev_expects;

    const usb_config_desc_t *config_desc;
    const usb_device_desc_t *device_desc;
    cdc_parsed_info_t cdc_info;
    usb_host_mock_get_config_descriptor_by_address(dev_address, &config_desc);
    usb_host_mock_get_device_descriptor_by_address(dev_address, &device_desc);
    cdc_parse_interface_descriptor(device_desc, config_desc, interface_index, &cdc_info);

    // Get IN and OUT endpoints addresses
    cdc_dev_expects->data.in_bEndpointAddress = cdc_info.in_ep->bEndpointAddress;
    cdc_dev_expects->data.out_bEndpointAddress = cdc_info.out_ep->bEndpointAddress;

    // Get notification endpoint address and check if notification transfer is allocated (if notif. EP exists)
    if (cdc_info.notif_ep) {
        cdc_dev_expects->notif.bEndpointAddress = cdc_info.notif_ep->bEndpointAddress;
        cdc_dev_expects->notif.has_xfer = true;
        if (cdc_info.notif_intf != cdc_info.data_intf) {
            cdc_dev_expects->notif.has_separate_interface = true;
        }
    } else {
        cdc_dev_expects->notif.bEndpointAddress = 0;
        cdc_dev_expects->notif.has_xfer = false;
    }

    // Check, if IN data transfer is allocated
    if (dev_config->in_buffer_size) {
        cdc_dev_expects->data.has_in = true;
    } else {
        cdc_dev_expects->data.has_in = false;
    }

    // Check if OUT data transfer is allocated
    if (dev_config->out_buffer_size) {
        cdc_dev_expects->data.has_out = true;
    } else {
        cdc_dev_expects->data.has_out = false;
    }

    for (const usb_intf_desc_t *intf : {
                cdc_info.data_intf, cdc_info.notif_intf != cdc_info.data_intf ? cdc_info.notif_intf : nullptr
            }) {
        if (!intf) {
            continue;
        }
        for (int i = 0; i < intf->bNumEndpoints; ++i) {
            int offset = reinterpret_cast<const uint8_t *>(intf) - reinterpret_cast<const uint8_t *>(config_desc);
            const auto *ep = usb_parse_endpoint_descriptor_by_index(intf, i, config_desc->wTotalLength, &offset);
            REQUIRE(ep != nullptr);
            cdc_dev_expects->reset_count += USB_EP_DESC_GET_XFERTYPE(ep) == USB_TRANSFER_TYPE_BULK || USB_EP_DESC_GET_XFERTYPE(ep) == USB_TRANSFER_TYPE_INTR;
        }
    }
    free(cdc_info.func);
    return ESP_OK;
}

esp_err_t test_cdc_acm_host_install(const cdc_acm_host_driver_config_t *driver_config)
{
    usb_host_client_register_ExpectAnyArgsAndReturn(ESP_OK);
    usb_host_client_register_AddCallback(register_client);
    usb_host_client_handle_events_Stub(handle_events);
    usb_host_client_unblock_Stub(usb_host_client_unblock_mock_callback);
    usb_host_transfer_submit_AddCallback(submit_transfer);
    usb_host_endpoint_flush_AddCallback(flush_endpoint);
    pending_transfers.clear();
    completed_transfers.clear();
    timed_out_control = nullptr;
    tx_response = MOCK_USB_TRANSFER_SUCCESS;

    // Call the real function cdc_acm_host_install()
    return cdc_acm_host_install(driver_config);
}

esp_err_t test_cdc_acm_host_uninstall(void)
{
    usb_host_client_deregister_ExpectAnyArgsAndReturn(ESP_OK);
    usb_host_client_deregister_AddCallback(usb_host_client_deregister_mock_callback);

    _test_cdc_expects_free_all();

    // Call the real function, cdc_acm_host_uninstall()
    return cdc_acm_host_uninstall();
}

esp_err_t test_cdc_acm_host_open(uint8_t dev_address, uint16_t vid, uint16_t pid, uint8_t interface_index, const cdc_acm_host_device_config_t *dev_config, cdc_acm_dev_hdl_t *cdc_hdl_ret)
{
    cdc_dev_expects_t *cdc_dev_expects = nullptr;
    esp_err_t ret = _test_create_cmock_expectations(dev_address, interface_index, dev_config, &cdc_dev_expects);
    if (ret != ESP_OK) {
        return ret;      // ESP_ERR_NO_MEM
    }

    // Basic USB Host stack operations are stubbed
    usb_host_device_addr_list_fill_Stub(usb_host_device_addr_list_fill_mock_callback);
    usb_host_device_info_Stub(usb_host_device_info_mock_callback);
    usb_host_get_device_descriptor_Stub(usb_host_get_device_descriptor_mock_callback);
    usb_host_get_active_config_descriptor_Stub(usb_host_get_active_config_descriptor_mock_callback);

    // We are expecting usb_host_device_open, usb_host_get_device_descriptor usb_host_device_close
    // to be called at least mocked_devs_count times
    const int mocked_devs_count = usb_host_mock_get_devs_count();   // Get number of mocked USB devices in mocked device list
    for (int i = 0; i < mocked_devs_count; i++) {
        usb_host_device_open_ExpectAnyArgsAndReturn(ESP_OK);
        usb_host_device_close_ExpectAnyArgsAndReturn(ESP_OK);
    }

    usb_host_device_open_AddCallback(usb_host_device_open_mock_callback);
    usb_host_device_close_AddCallback(usb_host_device_close_mock_callback);

    // Setup Notif transfer
    if (cdc_dev_expects->notif.has_xfer) {
        usb_host_transfer_alloc_ExpectAnyArgsAndReturn(ESP_OK);
    }

    // The 64-byte payload and 8-byte setup packet share the control transfer allocation.
    usb_host_transfer_alloc_ExpectAndReturn(64 + USB_SETUP_PACKET_SIZE, 0, nullptr, ESP_OK);
    usb_host_transfer_alloc_IgnoreArg_transfer();

    //  Setup IN data transfer
    if (dev_config->in_buffer_size) {
        usb_host_transfer_alloc_ExpectAnyArgsAndReturn(ESP_OK);
    }

    // Setup OUT bulk transfer
    if (dev_config->out_buffer_size) {
        usb_host_transfer_alloc_ExpectAnyArgsAndReturn(ESP_OK);
    }

    // Register callback
    usb_host_transfer_alloc_AddCallback(usb_host_transfer_alloc_mock_callback);

    // Call cdc_acm_start

    // Claim 1st interface
    // Make sure that the interface_index has been claimed
    const bool sep_notif = cdc_dev_expects->notif.has_separate_interface;
    test_usb_host_interface_claim(interface_index, sep_notif);

    // Claim 2nd interface (if supported)
    if (cdc_dev_expects->notif.has_separate_interface) {
        test_usb_host_interface_claim(interface_index + 1, sep_notif);
    } else if (cdc_dev_expects->notif.has_xfer) {
        // If the device has no separate interface, but has notification endpoint
        usb_host_transfer_submit_ExpectAnyArgsAndReturn(ESP_OK);
    }

    // Opening synchronizes every claimed endpoint before polling starts.
    for (unsigned i = 0; i < cdc_dev_expects->reset_count; i++) {
        usb_host_transfer_submit_control_ExpectAnyArgsAndReturn(ESP_OK);
    }
    usb_host_transfer_submit_control_AddCallback(usb_host_transfer_submit_control_success_mock_callback);

    // Call the real function cdc_acm_host_open
    // Expect ESP_OK and dev_hdl non nullptr
    ret = cdc_acm_host_open(vid, pid, interface_index, dev_config, cdc_hdl_ret);

    // If the cdc_acm_host_open() fails, delete the created cdc_device
    if (ret != ESP_OK) {
        free(cdc_dev_expects);
    } else {
        s_cdc_dev_expects_by_hdl[*cdc_hdl_ret] = cdc_dev_expects;
    }
    return ret;
}

esp_err_t test_cdc_acm_host_close(cdc_acm_dev_hdl_t *cdc_hdl, uint8_t interface_index)
{
    cdc_acm_dev_hdl_t hdl = *cdc_hdl;
    cdc_dev_expects_t *exp = _test_cdc_expects_lookup(hdl);
    if (exp == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    // Cancel pooling of IN endpoint -> halt, flush, clear
    test_cdc_acm_reset_transfer_endpoint(exp->data.in_bEndpointAddress);

    // Cancel OUT endpoint -> halt, flush, clear
    if (exp->data.has_out) {
        test_cdc_acm_reset_transfer_endpoint(exp->data.out_bEndpointAddress);
    }

    // Cancel pooling of Notification endpoint -> halt, flush, clear
    if (exp->notif.has_xfer) {
        test_cdc_acm_reset_transfer_endpoint(exp->notif.bEndpointAddress);
    }

    // Release data interface
    if (exp->notif.has_separate_interface) {
        usb_host_interface_release_ExpectAnyArgsAndReturn(ESP_OK);
        usb_host_interface_release_ExpectAnyArgsAndReturn(ESP_OK);
    } else {
        usb_host_interface_release_ExpectAndReturn(nullptr, nullptr, interface_index, ESP_OK);
    }
    usb_host_interface_release_IgnoreArg_client_hdl();  // Ignore all function parameters, except interface_index
    usb_host_interface_release_IgnoreArg_dev_hdl();


    // Free notif transfer
    if (exp->notif.has_xfer) {
        usb_host_transfer_free_ExpectAnyArgsAndReturn(ESP_OK);
    }

    // Free in transfer
    if (exp->data.has_in) {
        usb_host_transfer_free_ExpectAnyArgsAndReturn(ESP_OK);
    }

    // Free out transfer
    if (exp->data.has_out) {
        usb_host_transfer_free_ExpectAnyArgsAndReturn(ESP_OK);
    }

    // Call cdc_acm_device_remove
    // Free ctrl transfer
    usb_host_transfer_free_ExpectAnyArgsAndReturn(ESP_OK);
    usb_host_transfer_free_AddCallback(usb_host_transfer_free_mock_callback);

    usb_host_device_close_ExpectAnyArgsAndReturn(ESP_OK);

    // Call the real function cdc_acm_host_close
    esp_err_t ret = cdc_acm_host_close(*cdc_hdl);
    if (ret == ESP_OK) {
        _test_cdc_expects_free(hdl);
    }
    return ret;
}

esp_err_t test_cdc_acm_host_data_tx_blocking(cdc_acm_dev_hdl_t cdc_hdl, const uint8_t *data, size_t data_len, uint32_t timeout_ms, mock_usb_transfer_response_t transfer_response)
{
    cdc_dev_expects_t *exp = _test_cdc_expects_lookup(cdc_hdl);
    if (exp == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    usb_host_transfer_submit_ExpectAnyArgsAndReturn(ESP_OK);

    tx_response = transfer_response;
    if (transfer_response == MOCK_USB_TRANSFER_TIMEOUT) {
        test_cdc_acm_reset_transfer_endpoint(exp->data.out_bEndpointAddress);
    }

    // Call the real function cdc_acm_host_data_tx_blocking()
    return cdc_acm_host_data_tx_blocking(cdc_hdl, data, data_len, timeout_ms);
}

esp_err_t test_cdc_acm_reset_transfer_endpoint(uint8_t ep_address)
{
    // Expect correct ep_address for all (halt, flush, clear)
    usb_host_endpoint_halt_ExpectAndReturn(nullptr, ep_address, ESP_OK);
    usb_host_endpoint_halt_IgnoreArg_dev_hdl();

    usb_host_endpoint_flush_ExpectAndReturn(nullptr, ep_address, ESP_OK);
    usb_host_endpoint_flush_IgnoreArg_dev_hdl();

    usb_host_endpoint_clear_ExpectAndReturn(nullptr, ep_address, ESP_OK);
    usb_host_endpoint_clear_IgnoreArg_dev_hdl();

    return ESP_OK;
}

esp_err_t test_usb_host_interface_claim(uint8_t interface_index, bool has_separate_notification_interface)
{
    usb_host_interface_claim_ExpectAndReturn(nullptr, nullptr, interface_index, 0, ESP_OK);
    if (has_separate_notification_interface) {
        usb_host_interface_claim_IgnoreArg_bInterfaceNumber();
    }
    usb_host_interface_claim_IgnoreArg_client_hdl();        // Ignore all function parameters, except interface_index
    usb_host_interface_claim_IgnoreArg_dev_hdl();
    usb_host_interface_claim_IgnoreArg_bAlternateSetting();
    usb_host_transfer_submit_ExpectAnyArgsAndReturn(ESP_OK);

    return ESP_OK;
}
