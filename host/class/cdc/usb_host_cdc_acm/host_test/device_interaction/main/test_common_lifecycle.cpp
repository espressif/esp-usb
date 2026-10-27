/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <deque>
#include <functional>
#include <set>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "descriptors/cdc_descriptors.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "esp_bit_defs.h"
#include "mock_add_usb_device.h"
#include "usb/cdc_host_common.h"
#include "usb/cdc_acm_host.h"

extern "C" {
#include "Mockusb_host.h"
}

namespace {
struct Completion {
    usb_transfer_t *transfer;
    usb_transfer_status_t status;
    int length;
    bool reset = false;
};

struct CommonFixture {
    static CommonFixture *current;
    cdc_host_common_driver_handle_t driver = nullptr;
    cdc_host_common_port_handle_t port = nullptr;
    usb_host_client_handle_t client = nullptr;
    usb_host_client_event_cb_t device_callback = nullptr;
    void *device_arg = nullptr;
    EventGroupHandle_t events = xEventGroupCreate();
    std::vector<usb_transfer_t *> allocated;
    std::set<usb_transfer_t *> inflight;
    std::deque<Completion> completions;
    bool dispatching = false;
    bool hold_completions = false;
    bool safe_cleanup = true;
    bool disconnect_pending = false;
    bool close_from_data = false;
    bool close_from_notification = false;
    bool close_from_overflow = false;
    bool close_from_error = false;
    bool close_from_disconnect = false;
    bool new_device_pending = false;
    bool complete_out = false;
    uint8_t fail_endpoint = 0;
    unsigned fail_reset_submit = 0;
    unsigned fail_reset_completion = 0;
    unsigned timeout_reset = 0;
    unsigned disconnect_reset = 0;
    unsigned resume_reset = 0;
    usb_device_handle_t resume_device = nullptr;
    unsigned port_events = 0;
    unsigned completed_resets = 0;
    unsigned expected_resets = 3;
    bool shared_notification = false;
    bool reset_requests_valid = true;
    bool polls_after_reset = true;
    std::vector<uint8_t> reset_endpoints;
    unsigned fail_allocation = 0;
    unsigned fail_claim = 0;
    unsigned allocation_attempts = 0;
    unsigned claim_attempts = 0;
    unsigned device_opens = 0;
    unsigned device_closes = 0;
    unsigned submits = 0;
    unsigned frees = 0;
    unsigned overflows = 0;
    unsigned data_callbacks = 0;
    unsigned releases = 0;
    unsigned canceled = 0;
    unsigned errors = 0;
    int last_error = 0;
    std::set<uint8_t> claimed;
    std::vector<uint8_t> transmitted;
    std::function<void(cdc_host_common_port_handle_t, const uint8_t *, size_t)> on_data;
    std::function<void()> on_empty_ring;
    esp_err_t callback_close_result = ESP_FAIL;

    explicit CommonFixture(const uint8_t *config_desc = tusb_serial_device_config_desc_fs)
    {
        current = this;
        Mockusb_host_Init();
        usb_host_mock_dev_list_init();
        REQUIRE(ESP_OK == usb_host_mock_add_device(1, reinterpret_cast<const usb_device_desc_t *>(tusb_serial_device_device_desc_fs_hs),
                                                   reinterpret_cast<const usb_config_desc_t *>(config_desc), USB_SPEED_FULL));
        usb_host_client_register_Stub([](const usb_host_client_config_t *config, usb_host_client_handle_t *handle, int count) {
            current->device_callback = config->async.client_event_callback;
            current->device_arg = config->async.callback_arg;
            esp_err_t ret = usb_host_client_register_mock_callback(config, handle, count);
            current->client = *handle;
            return ret;
        });
        usb_host_client_handle_events_Stub([](usb_host_client_handle_t client, TickType_t ticks, int count) {
            usb_host_client_handle_events_mock_callback(client, ticks, count);
            auto &self = *current;
#ifdef CDC_HOST_COMMON_SUSPEND_RESUME_API_SUPPORTED
            if (self.resume_device) {
                usb_host_client_event_msg_t event = {};
                event.dev_suspend_resume.dev_hdl = self.resume_device;
                event.event = USB_HOST_CLIENT_EVENT_DEV_SUSPENDED;
                self.device_callback(&event, self.device_arg);
                event.event = USB_HOST_CLIENT_EVENT_DEV_RESUMED;
                self.device_callback(&event, self.device_arg);
                self.resume_device = nullptr;
            }
#endif
            while (!self.completions.empty() && (!self.hold_completions || self.completions.front().reset)) {
                Completion completion = self.completions.front();
                self.completions.pop_front();
                self.inflight.erase(completion.transfer);
                completion.transfer->status = completion.status;
                completion.transfer->actual_num_bytes = completion.length;
                if (completion.reset && completion.status == USB_TRANSFER_STATUS_COMPLETED) {
                    self.completed_resets++;
                }
                self.dispatching = true;
                completion.transfer->callback(completion.transfer);
                self.dispatching = false;
                if (completion.status == USB_TRANSFER_STATUS_CANCELED) {
                    self.canceled++;
                }
            }
            if (self.disconnect_pending) {
                self.disconnect_pending = false;
                usb_device_handle_t device = self.allocated.front()->device_handle;
                usb_host_client_event_msg_t event = {};
                event.event = USB_HOST_CLIENT_EVENT_DEV_GONE;
                event.dev_gone.dev_hdl = device;
                self.dispatching = true;
                self.device_callback(&event, self.device_arg);
                self.dispatching = false;
                xEventGroupSetBits(self.events, BIT1);
            }
            if (self.new_device_pending) {
                self.new_device_pending = false;
                usb_host_client_event_msg_t event = {};
                event.event = USB_HOST_CLIENT_EVENT_NEW_DEV;
                event.new_dev.address = 1;
                self.dispatching = true;
                self.device_callback(&event, self.device_arg);
                self.dispatching = false;
            }
            xEventGroupSetBits(self.events, BIT0);
            return ESP_OK;
        });
        usb_host_client_unblock_Stub(usb_host_client_unblock_mock_callback);
        usb_host_client_deregister_Stub(usb_host_client_deregister_mock_callback);
        usb_host_device_addr_list_fill_Stub(usb_host_device_addr_list_fill_mock_callback);
        usb_host_device_open_Stub([](usb_host_client_handle_t client, uint8_t address, usb_device_handle_t *device, int count) {
            esp_err_t ret = usb_host_device_open_mock_callback(client, address, device, count);
            current->device_opens += ret == ESP_OK;
            return ret;
        });
        usb_host_device_close_Stub([](usb_host_client_handle_t client, usb_device_handle_t device, int count) {
            current->device_closes++;
            return usb_host_device_close_mock_callback(client, device, count);
        });
        usb_host_device_info_Stub(usb_host_device_info_mock_callback);
        usb_host_get_device_descriptor_Stub(usb_host_get_device_descriptor_mock_callback);
        usb_host_get_active_config_descriptor_Stub(usb_host_get_active_config_descriptor_mock_callback);
        usb_host_interface_claim_Stub([](usb_host_client_handle_t, usb_device_handle_t, uint8_t interface, uint8_t, int) {
            auto &self = *current;
            if (++self.claim_attempts == self.fail_claim) {
                return ESP_ERR_NOT_FOUND;
            }
            self.claimed.insert(interface);
            return ESP_OK;
        });
        usb_host_interface_release_Stub([](usb_host_client_handle_t, usb_device_handle_t, uint8_t interface, int) {
            current->safe_cleanup &= !current->dispatching && current->inflight.empty();
            current->safe_cleanup &= current->claimed.erase(interface) == 1;
            current->releases++;
            return ESP_OK;
        });
        usb_host_transfer_alloc_Stub([](size_t size, int packets, usb_transfer_t **transfer, int count) {
            if (++current->allocation_attempts == current->fail_allocation) {
                return ESP_ERR_NO_MEM;
            }
            esp_err_t ret = usb_host_transfer_alloc_mock_callback(size, packets, transfer, count);
            if (ret == ESP_OK) {
                current->allocated.push_back(*transfer);
            }
            return ret;
        });
        usb_host_transfer_free_Stub([](usb_transfer_t *transfer, int count) {
            auto &self = *current;
            self.safe_cleanup &= !self.dispatching && self.inflight.count(transfer) == 0;
            self.frees++;
            self.allocated.erase(std::remove(self.allocated.begin(), self.allocated.end(), transfer), self.allocated.end());
            return usb_host_transfer_free_mock_callback(transfer, count);
        });
        usb_host_transfer_submit_Stub([](usb_transfer_t *transfer, int) {
            auto &self = *current;
            if (transfer->bEndpointAddress == self.fail_endpoint) {
                return ESP_FAIL;
            }
            self.polls_after_reset &= self.completed_resets == self.expected_resets;
            self.submits++;
            self.inflight.insert(transfer);
            if (transfer->bEndpointAddress == 0x02) {
                self.transmitted.insert(self.transmitted.end(), transfer->data_buffer, transfer->data_buffer + transfer->num_bytes);
                if (self.complete_out) {
                    self.completions.push_back({transfer, USB_TRANSFER_STATUS_COMPLETED, transfer->num_bytes});
                    usb_host_client_unblock_mock_callback(self.client, 0);
                }
            }
            return ESP_OK;
        });
        usb_host_transfer_submit_control_Stub([](usb_host_client_handle_t, usb_transfer_t *transfer, int) {
            auto &self = *current;
            const auto *request = reinterpret_cast<const usb_setup_packet_t *>(transfer->data_buffer);
            bool reset = request->bmRequestType == 0x02 && request->bRequest == 0x01;
            if (reset) {
                self.reset_requests_valid &= request->wValue == 0 && request->wLength == 0 && (request->wIndex & 0xFF00) == 0 && transfer->num_bytes == USB_SETUP_PACKET_SIZE;
                self.reset_endpoints.push_back(request->wIndex);
                unsigned attempt = self.reset_endpoints.size();
                if (attempt == self.fail_reset_submit) {
                    return ESP_FAIL;
                }
                self.inflight.insert(transfer);
                if (attempt == self.resume_reset) {
                    self.resume_device = transfer->device_handle;
                }
                if (attempt == self.disconnect_reset) {
                    self.disconnect_pending = true;
                } else if (attempt != self.timeout_reset) {
                    usb_transfer_status_t status = attempt == self.fail_reset_completion ? USB_TRANSFER_STATUS_STALL : USB_TRANSFER_STATUS_COMPLETED;
                    self.completions.push_back({transfer, status, USB_SETUP_PACKET_SIZE, true});
                }
                usb_host_client_unblock_mock_callback(self.client, 0);
            } else {
                self.inflight.insert(transfer);
            }
            return ESP_OK;
        });
        usb_host_endpoint_halt_IgnoreAndReturn(ESP_OK);
        usb_host_endpoint_clear_IgnoreAndReturn(ESP_OK);
        usb_host_endpoint_flush_Stub([](usb_device_handle_t, uint8_t endpoint, int) {
            auto &self = *current;
            for (auto *transfer : self.inflight) {
                if (transfer->bEndpointAddress == endpoint && std::none_of(self.completions.begin(), self.completions.end(),
                [transfer](const Completion & item) {
                return item.transfer == transfer;
            })) {
                    self.completions.push_back({transfer, USB_TRANSFER_STATUS_CANCELED, 0});
                }
            }
            usb_host_client_unblock_mock_callback(self.client, 0);
            return ESP_OK;
        });
        REQUIRE(ESP_OK == cdc_host_common_acquire(nullptr, &driver));
    }

    ~CommonFixture()
    {
        if (port) {
            cdc_host_common_close(port);
        }
        if (driver) {
            cdc_host_common_release(driver);
        }
        vEventGroupDelete(events);
        Mockusb_host_Destroy();
        current = nullptr;
    }

    esp_err_t open(size_t rx_size = 0, size_t tx_size = 0, uint32_t flags = 0, size_t in_size = 64, size_t out_size = 64)
    {
        cdc_host_common_open_config_t config = {};
        config.vid = CDC_HOST_COMMON_ANY_VID;
        config.pid = CDC_HOST_COMMON_ANY_PID;
        config.dev_addr = 1;
        config.connection_timeout_ms = 100;
        config.in_buffer_size = in_size;
        config.out_buffer_size = out_size;
        config.flags = static_cast<cdc_host_common_open_flags_t>(flags);
        expected_resets = (flags & CDC_HOST_COMMON_OPEN_FLAG_DISABLE_NOTIFICATION) && !shared_notification ? 2 : 3;
        completed_resets = 0;
        reset_endpoints.clear();
        config.rx_ringbuf_size = rx_size;
        config.tx_ringbuf_size = tx_size;
        config.data_cb = [](cdc_host_common_port_handle_t handle, const uint8_t *data, size_t length, void *) {
            current->data_callbacks++;
            if (current->on_data) {
                current->on_data(handle, data, length);
            }
            if (current->close_from_data) {
                current->callback_close_result = cdc_host_common_close(handle);
                current->port = nullptr;
            }
            return true;
        };
        config.event_cb = [](cdc_host_common_port_handle_t handle, const cdc_host_common_port_event_data_t *event, void *) {
            current->port_events++;
            if (event->type == CDC_HOST_COMMON_PORT_EVENT_RX_OVERFLOW) {
                current->overflows++;
            }
            if (event->type == CDC_HOST_COMMON_PORT_EVENT_ERROR) {
                current->errors++;
                current->last_error = event->data.error;
            }
            if ((event->type == CDC_HOST_COMMON_PORT_EVENT_NOTIFICATION && current->close_from_notification) ||
                    (event->type == CDC_HOST_COMMON_PORT_EVENT_RX_OVERFLOW && current->close_from_overflow) ||
                    (event->type == CDC_HOST_COMMON_PORT_EVENT_ERROR && current->close_from_error) ||
                    (event->type == CDC_HOST_COMMON_PORT_EVENT_DISCONNECTED && current->close_from_disconnect)) {
                current->callback_close_result = cdc_host_common_close(handle);
                current->port = nullptr;
            }
        };
        if (!in_size && !rx_size) {
            config.data_cb = nullptr;
        }
        return cdc_host_common_open(driver, &config, &port);
    }

    usb_transfer_t *transfer(uint8_t endpoint)
    {
        auto found = std::find_if(allocated.begin(), allocated.end(), [endpoint](usb_transfer_t *item) {
            return item->bEndpointAddress == endpoint;
        });
        REQUIRE(found != allocated.end());
        return *found;
    }

    void complete(uint8_t endpoint, usb_transfer_status_t status, int length)
    {
        xEventGroupClearBits(events, BIT0);
        completions.push_back({transfer(endpoint), status, length});
        usb_host_client_unblock_mock_callback(client, 0);
        REQUIRE(xEventGroupWaitBits(events, BIT0, pdTRUE, pdTRUE, pdMS_TO_TICKS(1000)) & BIT0);
    }

    void receive(uint8_t endpoint, int length)
    {
        complete(endpoint, USB_TRANSFER_STATUS_COMPLETED, length);
    }

    void receive(const std::vector<uint8_t> &data)
    {
        auto *input = transfer(0x82);
        REQUIRE(data.size() <= static_cast<size_t>(input->num_bytes));
        std::copy(data.begin(), data.end(), input->data_buffer);
        receive(0x82, data.size());
    }

    void notify_new_device()
    {
        xEventGroupClearBits(events, BIT0);
        new_device_pending = true;
        usb_host_client_unblock_mock_callback(client, 0);
        REQUIRE(xEventGroupWaitBits(events, BIT0, pdTRUE, pdTRUE, pdMS_TO_TICKS(1000)) & BIT0);
    }

    void close()
    {
        REQUIRE(ESP_OK == cdc_host_common_close(port));
        port = nullptr;
        REQUIRE(safe_cleanup);
        REQUIRE(inflight.empty());
        REQUIRE(allocated.empty());
        REQUIRE(claimed.empty());
    }
};

CommonFixture *CommonFixture::current = nullptr;
}

extern "C" void __real_vRingbufferGetInfo(RingbufHandle_t, UBaseType_t *, UBaseType_t *, UBaseType_t *, UBaseType_t *, UBaseType_t *);
extern "C" void __wrap_vRingbufferGetInfo(RingbufHandle_t ring, UBaseType_t *free, UBaseType_t *read, UBaseType_t *write, UBaseType_t *acquire, UBaseType_t *waiting)
{
    __real_vRingbufferGetInfo(ring, free, read, write, acquire, waiting);
    if (CommonFixture::current && waiting && *waiting == 0 && CommonFixture::current->on_empty_ring) {
        std::exchange(CommonFixture::current->on_empty_ring, nullptr)();
    }
}

TEST_CASE("Common open rollback waits for submitted polls", "[common][lifecycle]")
{
    CommonFixture fixture;
    SECTION("BULK IN submit fails") {
        fixture.fail_endpoint = 0x82;
        REQUIRE(ESP_FAIL == fixture.open());
        REQUIRE(fixture.canceled == 0);
    }
    SECTION("notification submit fails after BULK IN") {
        fixture.fail_endpoint = 0x81;
        REQUIRE(ESP_FAIL == fixture.open());
        REQUIRE(fixture.canceled == 1);
    }
    SECTION("delayed cancellation after rollback timeout still releases the abandoned port") {
        fixture.fail_endpoint = 0x81;
        fixture.hold_completions = true;
        REQUIRE(ESP_FAIL == fixture.open());
        REQUIRE(fixture.frees == 0);
        xEventGroupClearBits(fixture.events, BIT0);
        fixture.hold_completions = false;
        usb_host_client_unblock_mock_callback(fixture.client, 0);
        REQUIRE(xEventGroupWaitBits(fixture.events, BIT0, pdTRUE, pdTRUE, pdMS_TO_TICKS(1000)) & BIT0);
        REQUIRE(fixture.canceled == 1);
    }
    REQUIRE(fixture.port == nullptr);
    REQUIRE(fixture.allocated.empty());
    REQUIRE(fixture.inflight.empty());
    REQUIRE(fixture.data_callbacks == 0);
    REQUIRE(fixture.safe_cleanup);
    REQUIRE(fixture.releases == 2);
}

TEST_CASE("Common callbacks may close without resubmitting", "[common][lifecycle]")
{
    CommonFixture fixture;
    REQUIRE(ESP_OK == fixture.open(64));
    SECTION("data callback") {
        fixture.close_from_data = true;
        fixture.receive(0x82, 32);
    }
    SECTION("notification callback") {
        fixture.close_from_notification = true;
        fixture.receive(0x81, 8);
    }
    SECTION("overflow callback suppresses the later data callback") {
        fixture.receive(0x82, 64);
        fixture.close_from_overflow = true;
        fixture.receive(0x82, 1);
        REQUIRE(fixture.data_callbacks == 1);
        REQUIRE(fixture.overflows == 1);
    }
    REQUIRE(fixture.callback_close_result == ESP_OK);
    unsigned submits = fixture.submits;
    REQUIRE(ESP_OK == cdc_host_common_release(fixture.driver));
    fixture.driver = nullptr;
    REQUIRE(fixture.safe_cleanup);
    REQUIRE(fixture.allocated.empty());
    REQUIRE(fixture.submits == submits);
}

TEST_CASE("Common buffered RX accepts exact capacity and reports dropped chunks", "[common][rx]")
{
    CommonFixture fixture;
    REQUIRE(ESP_OK == fixture.open(64));
    fixture.receive(0x82, 64);
    size_t size = 0;
    REQUIRE(ESP_OK == cdc_host_common_get_rx_buffer_size(fixture.port, &size));
    REQUIRE(size == 64);
    REQUIRE(fixture.overflows == 0);
    fixture.receive(0x82, 1);
    REQUIRE(fixture.overflows == 1);

    uint8_t data[64];
    size = 32;
    REQUIRE(ESP_OK == cdc_host_common_read_bytes(fixture.port, data, &size, 0));
    fixture.receive(0x82, 32);
    REQUIRE(fixture.overflows == 1);
    size = 31;
    REQUIRE(ESP_OK == cdc_host_common_read_bytes(fixture.port, data, &size, 0));
    fixture.receive(0x82, 32);
    REQUIRE(fixture.overflows == 2);
    REQUIRE(ESP_OK == cdc_host_common_get_rx_buffer_size(fixture.port, &size));
    REQUIRE(size == 33);
    fixture.close();
}

TEST_CASE("Common disconnect keeps its handle until explicit close", "[common][lifecycle]")
{
    CommonFixture fixture;
    REQUIRE(ESP_OK == fixture.open());
    const usb_intf_desc_t *data_interface = nullptr;
    REQUIRE(ESP_OK == cdc_host_common_get_intf_desc(fixture.port, nullptr, &data_interface));
    uint8_t interface_number = data_interface->bInterfaceNumber;
    fixture.disconnect_pending = true;
    usb_host_client_unblock_mock_callback(fixture.client, 0);
    REQUIRE(xEventGroupWaitBits(fixture.events, BIT1, pdTRUE, pdTRUE, pdMS_TO_TICKS(1000)) & BIT1);
    size_t size;
    REQUIRE(ESP_ERR_INVALID_STATE == cdc_host_common_get_rx_buffer_size(fixture.port, &size));
    REQUIRE(ESP_ERR_INVALID_STATE == cdc_host_common_release(fixture.driver));
    REQUIRE(data_interface->bInterfaceNumber == interface_number);
    fixture.close();
}

TEST_CASE("Common close interrupts an infinite buffered read", "[common][lifecycle]")
{
    CommonFixture fixture;
    REQUIRE(ESP_OK == fixture.open(64));
    struct Reader {
        cdc_host_common_port_handle_t port;
        SemaphoreHandle_t started;
        SemaphoreHandle_t done;
        esp_err_t result;
    } reader = {fixture.port, xSemaphoreCreateBinary(), xSemaphoreCreateBinary(), ESP_FAIL};
    REQUIRE(pdPASS == xTaskCreate([](void *arg) {
        auto *reader = static_cast<Reader *>(arg);
        uint8_t data[8];
        size_t size = sizeof(data);
        xSemaphoreGive(reader->started);
        reader->result = cdc_host_common_read_bytes(reader->port, data, &size, portMAX_DELAY);
        xSemaphoreGive(reader->done);
        vTaskDelete(nullptr);
    }, "common_read", 4096, &reader, 5, nullptr));
    REQUIRE(pdTRUE == xSemaphoreTake(reader.started, pdMS_TO_TICKS(1000)));
    fixture.close();
    REQUIRE(pdTRUE == xSemaphoreTake(reader.done, pdMS_TO_TICKS(1000)));
    REQUIRE(reader.result == ESP_ERR_INVALID_STATE);
    vSemaphoreDelete(reader.started);
    vSemaphoreDelete(reader.done);
}

TEST_CASE("Common close retains ownership while control transfer is pending", "[common][lifecycle]")
{
    CommonFixture fixture;
    REQUIRE(ESP_OK == fixture.open());
    REQUIRE(ESP_ERR_TIMEOUT == cdc_host_common_send_control(fixture.port, 0, 0, 0, 0, 0, nullptr));
    SECTION("failed synchronous close preserves the handle for retry") {
        REQUIRE(ESP_ERR_TIMEOUT == cdc_host_common_close(fixture.port));
        REQUIRE(fixture.frees == 0);
        fixture.receive(0, 0);
        size_t size;
        REQUIRE(ESP_ERR_INVALID_STATE == cdc_host_common_get_rx_buffer_size(fixture.port, &size));
        fixture.close();
    }
    SECTION("asynchronous close transfers ownership from an application task") {
        REQUIRE(ESP_OK == cdc_host_common_close_async(fixture.port));
        fixture.port = nullptr;
        REQUIRE(ESP_ERR_TIMEOUT == cdc_host_common_release(fixture.driver));
        fixture.receive(0, 0);
        REQUIRE(ESP_OK == cdc_host_common_release(fixture.driver));
        fixture.driver = nullptr;
        REQUIRE(fixture.allocated.empty());
        REQUIRE(fixture.safe_cleanup);
    }
    SECTION("release timeout preserves an asynchronous close request") {
        fixture.close_from_data = true;
        fixture.receive(0x82, 1);
        REQUIRE(fixture.callback_close_result == ESP_OK);
        REQUIRE(ESP_ERR_TIMEOUT == cdc_host_common_release(fixture.driver));
        fixture.receive(0, 0);
        REQUIRE(ESP_OK == cdc_host_common_release(fixture.driver));
        fixture.driver = nullptr;
        REQUIRE(fixture.allocated.empty());
        REQUIRE(fixture.safe_cleanup);
    }
}

TEST_CASE("Common allocation failures release partial resources and allow reopening", "[common][fault]")
{
    CommonFixture fixture;
    SECTION("notification allocation") {
        fixture.fail_allocation = 1;
    }
    SECTION("control allocation") {
        fixture.fail_allocation = 2;
    }
    SECTION("BULK IN allocation") {
        fixture.fail_allocation = 3;
    }
    SECTION("BULK OUT allocation") {
        fixture.fail_allocation = 4;
    }
    REQUIRE(ESP_ERR_NO_MEM == fixture.open(64, 64));
    REQUIRE(fixture.port == nullptr);
    REQUIRE(fixture.frees == fixture.fail_allocation - 1);
    REQUIRE(fixture.allocated.empty());
    REQUIRE(fixture.claimed.empty());
    REQUIRE(fixture.inflight.empty());
    REQUIRE(fixture.device_opens == fixture.device_closes);
    REQUIRE(fixture.safe_cleanup);

    fixture.fail_allocation = 0;
    REQUIRE(ESP_OK == fixture.open(64, 64));
    fixture.close();
    REQUIRE(fixture.device_opens == fixture.device_closes);
}

TEST_CASE("Common claim failures release only claimed interfaces and allow reopening", "[common][fault]")
{
    CommonFixture fixture;
    SECTION("data interface rejected") {
        fixture.fail_claim = 1;
    }
    SECTION("notification interface rejected after data claim") {
        fixture.fail_claim = 2;
    }
    REQUIRE(ESP_ERR_NOT_FOUND == fixture.open(64, 64));
    REQUIRE(fixture.port == nullptr);
    REQUIRE(fixture.frees == 4);
    REQUIRE(fixture.releases == fixture.fail_claim - 1);
    REQUIRE(fixture.claimed.empty());
    REQUIRE(fixture.allocated.empty());
    REQUIRE(fixture.submits == 0);
    REQUIRE(fixture.device_opens == fixture.device_closes);
    REQUIRE(fixture.safe_cleanup);

    fixture.fail_claim = 0;
    REQUIRE(ESP_OK == fixture.open());
    fixture.close();
}

TEST_CASE("Common blocking TX recovers from submit failure and preserves chunk order", "[common][fault][tx]")
{
    CommonFixture fixture;
    REQUIRE(ESP_OK == fixture.open());
    std::vector<uint8_t> data(145);
    for (size_t i = 0; i < data.size(); i++) {
        data[i] = static_cast<uint8_t>(i * 7);
    }
    fixture.fail_endpoint = 0x02;
    REQUIRE(ESP_FAIL == cdc_host_common_write_bytes(fixture.port, data.data(), data.size(), pdMS_TO_TICKS(1000)));
    REQUIRE(fixture.transmitted.empty());

    fixture.fail_endpoint = 0;
    fixture.complete_out = true;
    REQUIRE(ESP_OK == cdc_host_common_write_bytes(fixture.port, data.data(), data.size(), pdMS_TO_TICKS(1000)));
    REQUIRE(fixture.transmitted == data);
    REQUIRE(fixture.inflight.count(fixture.transfer(0x02)) == 0);
    fixture.close();
}

TEST_CASE("Common buffered TX recovers from rejected submission without replaying it", "[common][fault][tx]")
{
    CommonFixture fixture;
    REQUIRE(ESP_OK == fixture.open(0, 64));
    const std::vector<uint8_t> rejected(16, 0xa5);
    const std::vector<uint8_t> accepted(32, 0x5a);
    fixture.fail_endpoint = 0x02;
    REQUIRE(ESP_FAIL == cdc_host_common_write_bytes(fixture.port, rejected.data(), rejected.size(), 0));
    REQUIRE(fixture.transmitted.empty());

    fixture.fail_endpoint = 0;
    REQUIRE(ESP_OK == cdc_host_common_write_bytes(fixture.port, accepted.data(), accepted.size(), 0));
    REQUIRE(fixture.transmitted == accepted);
    REQUIRE(fixture.inflight.count(fixture.transfer(0x02)) == 1);
    fixture.receive(0x02, accepted.size());
    REQUIRE(ESP_OK == cdc_host_common_flush_tx_buffer(fixture.port));
    fixture.close();
}

TEST_CASE("Common buffered TX handles enqueue during the idle transition", "[common][tx]")
{
    CommonFixture fixture;
    REQUIRE(ESP_OK == fixture.open(0, 64));
    const uint8_t first = 0x11, second = 0x22;
    REQUIRE(ESP_OK == cdc_host_common_write_bytes(fixture.port, &first, 1, 0));
    // Run the competing write after the callback observes empty, before it publishes idle.
    fixture.on_empty_ring = [&]() {
        REQUIRE(ESP_OK == cdc_host_common_write_bytes(fixture.port, &second, 1, 0));
    };
    fixture.receive(0x02, 1);
    REQUIRE(fixture.transmitted == std::vector<uint8_t> {first, second});
    REQUIRE(fixture.inflight.count(fixture.transfer(0x02)) == 1);
    fixture.close();
}

TEST_CASE("Common control requests reject incorrect lengths without copying stale bytes", "[common][control]")
{
    CommonFixture fixture;
    REQUIRE(ESP_OK == fixture.open());
    static int reply_length;
    uint8_t request_type = 0xA1;
    SECTION("IN response") {}
    SECTION("OUT response") {
        request_type = 0x21;
    }
    usb_host_transfer_submit_control_Stub([](usb_host_client_handle_t, usb_transfer_t *transfer, int) {
        transfer->status = USB_TRANSFER_STATUS_COMPLETED;
        transfer->actual_num_bytes = reply_length;
        transfer->callback(transfer);
        return ESP_OK;
    });
    const int reply_lengths[] = {0, 8, 9, 14, 15, 16};
    for (int length : reply_lengths) {
        reply_length = length;
        std::vector<uint8_t> data(7, 0x11);
        auto *transfer = fixture.transfer(0);
        std::fill(transfer->data_buffer + USB_SETUP_PACKET_SIZE, transfer->data_buffer + transfer->data_buffer_size, 0xA5);
        const bool complete = length == USB_SETUP_PACKET_SIZE + data.size();
        const esp_err_t expected = complete ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        REQUIRE(cdc_host_common_send_control(fixture.port, request_type, 0x21, 0, 0, data.size(), data.data()) == expected);
        REQUIRE(data == std::vector<uint8_t>(7, complete && (request_type & USB_BM_REQUEST_TYPE_DIR_IN) ? 0xA5 : 0x11));
    }
    fixture.close();
}

TEST_CASE("ACM preserves optional RX polling", "[common][acm][rx]")
{
    CommonFixture fixture;
    REQUIRE(ESP_OK == cdc_acm_host_install(nullptr));
    cdc_acm_host_open_config_t config = {};
    config.dev_addr = 1;
    config.connection_timeout_ms = 100;
    config.out_buffer_size = 64;
    SECTION("Zero buffer without callback disables RX") {}
    SECTION("Callback enables MPS fallback") {
        config.data_cb = [](const uint8_t *, size_t, void *) {
            return true;
        };
    }
    SECTION("Explicit buffer enables RX without callback") {
        config.in_buffer_size = 64;
    }
    cdc_acm_dev_hdl_t device = nullptr;
    REQUIRE(ESP_OK == cdc_acm_host_open(&config, &device));
    const bool polling = std::any_of(fixture.inflight.begin(), fixture.inflight.end(), [](const usb_transfer_t *transfer) {
        return transfer->bEndpointAddress == 0x82;
    });
    REQUIRE(polling == (config.data_cb != nullptr || config.in_buffer_size != 0));
    REQUIRE(ESP_OK == cdc_acm_host_close(device));
    REQUIRE(ESP_OK == cdc_acm_host_uninstall());
}

TEST_CASE("Common buffered RX preserves byte order across ring wrap", "[common][rx]")
{
    CommonFixture fixture;
    REQUIRE(ESP_OK == fixture.open(64));
    std::vector<uint8_t> first(48), second(48);
    for (size_t i = 0; i < first.size(); i++) {
        first[i] = static_cast<uint8_t>(i);
        second[i] = static_cast<uint8_t>(i + first.size());
    }
    fixture.receive(first);
    uint8_t data[128] = {};
    size_t size = 40;
    REQUIRE(ESP_OK == cdc_host_common_read_bytes(fixture.port, data, &size, 0));
    REQUIRE(size == 40);
    REQUIRE(std::equal(first.begin(), first.begin() + size, data));
    fixture.receive(second);

    size = sizeof(data);
    REQUIRE(ESP_OK == cdc_host_common_read_bytes(fixture.port, data, &size, 0));
    REQUIRE(size == 56);
    std::vector<uint8_t> expected(first.begin() + 40, first.end());
    expected.insert(expected.end(), second.begin(), second.end());
    REQUIRE(std::vector<uint8_t>(data, data + size) == expected);
    REQUIRE(fixture.overflows == 0);
    fixture.close();
}

TEST_CASE("Common RX flush removes both wrapped segments and accepts fresh data", "[common][rx]")
{
    CommonFixture fixture;
    REQUIRE(ESP_OK == fixture.open(64));
    fixture.receive(std::vector<uint8_t>(48, 0x11));
    uint8_t data[64] = {};
    size_t size = 40;
    REQUIRE(ESP_OK == cdc_host_common_read_bytes(fixture.port, data, &size, 0));
    fixture.receive(std::vector<uint8_t>(48, 0x22));
    REQUIRE(ESP_OK == cdc_host_common_flush_rx_buffer(fixture.port));
    REQUIRE(ESP_OK == cdc_host_common_get_rx_buffer_size(fixture.port, &size));
    REQUIRE(size == 0);
    size = sizeof(data);
    REQUIRE(ESP_ERR_TIMEOUT == cdc_host_common_read_bytes(fixture.port, data, &size, 0));
    REQUIRE(size == 0);
    REQUIRE(ESP_OK == cdc_host_common_flush_rx_buffer(fixture.port));

    const std::vector<uint8_t> fresh(64, 0x33);
    fixture.receive(fresh);
    size = sizeof(data);
    REQUIRE(ESP_OK == cdc_host_common_read_bytes(fixture.port, data, &size, 0));
    REQUIRE(std::vector<uint8_t>(data, data + size) == fresh);
    fixture.close();
}

TEST_CASE("Common RX overflow drops the whole chunk without corrupting queued bytes", "[common][rx]")
{
    CommonFixture fixture;
    REQUIRE(ESP_OK == fixture.open(64));
    const std::vector<uint8_t> first(48, 0x17), rejected(17, 0xee), last(16, 0x6c);
    fixture.receive(first);
    fixture.receive(rejected);
    REQUIRE(fixture.overflows == 1);
    fixture.receive(last);
    REQUIRE(fixture.overflows == 1);

    uint8_t data[64];
    size_t size = sizeof(data);
    REQUIRE(ESP_OK == cdc_host_common_read_bytes(fixture.port, data, &size, 0));
    std::vector<uint8_t> expected = first;
    expected.insert(expected.end(), last.begin(), last.end());
    REQUIRE(std::vector<uint8_t>(data, data + size) == expected);
    fixture.close();
}

TEST_CASE("Common zero-length RX completion keeps polling without occupying the ring", "[common][rx]")
{
    CommonFixture fixture;
    REQUIRE(ESP_OK == fixture.open(64));
    unsigned submits = fixture.submits;
    fixture.receive(0x82, 0);
    REQUIRE(fixture.submits == submits + 1);
    size_t size = 1;
    REQUIRE(ESP_OK == cdc_host_common_get_rx_buffer_size(fixture.port, &size));
    REQUIRE(size == 0);
    const std::vector<uint8_t> fresh(64, 0xc3);
    fixture.receive(fresh);
    uint8_t data[64];
    size = sizeof(data);
    REQUIRE(ESP_OK == cdc_host_common_read_bytes(fixture.port, data, &size, 0));
    REQUIRE(std::vector<uint8_t>(data, data + size) == fresh);
    REQUIRE(fixture.overflows == 0);
    fixture.close();
}

TEST_CASE("Common unbuffered RX data is readable during its callback only", "[common][rx]")
{
    CommonFixture fixture;
    REQUIRE(ESP_OK == fixture.open());
    std::vector<uint8_t> copied;
    esp_err_t peek_result = ESP_FAIL, read_result = ESP_FAIL;
    bool same_chunk = false;
    fixture.on_data = [&](cdc_host_common_port_handle_t port, const uint8_t *data, size_t length) {
        const uint8_t *peek = nullptr;
        size_t size = 0;
        peek_result = cdc_host_common_get_rx_data(port, &peek, &size);
        same_chunk = peek == data && size == length;
        copied.resize(length);
        read_result = cdc_host_common_read_bytes(port, copied.data(), &size, 0);
        copied.resize(size);
    };
    const std::vector<uint8_t> payload = {0x00, 0xff, 0x10, 0x80, 0x55, 0xaa};
    fixture.receive(payload);
    REQUIRE(peek_result == ESP_OK);
    REQUIRE(read_result == ESP_OK);
    REQUIRE(same_chunk);
    REQUIRE(copied == payload);
    const uint8_t *peek = nullptr;
    size_t size = 0;
    REQUIRE(ESP_ERR_INVALID_STATE == cdc_host_common_get_rx_data(fixture.port, &peek, &size));
    REQUIRE(ESP_OK == cdc_host_common_get_rx_buffer_size(fixture.port, &size));
    REQUIRE(size == 0);
    fixture.close();
}

TEST_CASE("Common device callback may unregister itself while other subscribers continue", "[common][lifecycle]")
{
    CommonFixture fixture;
    struct Observer {
        cdc_host_common_dev_event_cb_handle_t handle = nullptr;
        unsigned calls = 0;
        esp_err_t result = ESP_FAIL;
        bool valid_descriptors = false;
    } observer;
    cdc_host_common_dev_event_cb_handle_t other = nullptr;
    unsigned other_calls = 0;
    REQUIRE(ESP_OK == cdc_host_common_register_dev_event_cb(fixture.driver, [](const cdc_host_common_dev_event_data_t *, void *arg) {
        (*static_cast<unsigned *>(arg))++;
    }, &other_calls, &other));
    REQUIRE(ESP_OK == cdc_host_common_register_dev_event_cb(fixture.driver, [](const cdc_host_common_dev_event_data_t *event, void *arg) {
        auto &observer = *static_cast<Observer *>(arg);
        observer.calls++;
        observer.valid_descriptors = event->type == CDC_HOST_COMMON_DEV_EVENT_NEW && event->data.new_dev.dev_addr == 1 &&
                                     event->data.new_dev.device_desc && event->data.new_dev.config_desc;
        observer.result = cdc_host_common_unregister_dev_event_cb(observer.handle);
        observer.handle = nullptr;
    }, &observer, &observer.handle));
    fixture.notify_new_device();
    fixture.notify_new_device();
    REQUIRE(observer.calls == 1);
    REQUIRE(observer.result == ESP_OK);
    REQUIRE(observer.valid_descriptors);
    REQUIRE(other_calls == 2);
    REQUIRE(ESP_OK == cdc_host_common_unregister_dev_event_cb(other));
    REQUIRE(fixture.device_opens == fixture.device_closes);
}

TEST_CASE("Common device callback may unregister another subscriber during dispatch", "[common][lifecycle]")
{
    CommonFixture fixture;
    struct Observer {
        cdc_host_common_dev_event_cb_handle_t handle = nullptr;
        Observer *other = nullptr;
        unsigned calls = 0;
        esp_err_t result = ESP_FAIL;
    } first, second;
    first.other = &second;
    second.other = &first;
    auto callback = [](const cdc_host_common_dev_event_data_t *, void *arg) {
        auto &observer = *static_cast<Observer *>(arg);
        observer.calls++;
        if (observer.other->handle) {
            observer.result = cdc_host_common_unregister_dev_event_cb(observer.other->handle);
            observer.other->handle = nullptr;
        }
    };
    REQUIRE(ESP_OK == cdc_host_common_register_dev_event_cb(fixture.driver, callback, &first, &first.handle));
    REQUIRE(ESP_OK == cdc_host_common_register_dev_event_cb(fixture.driver, callback, &second, &second.handle));
    fixture.notify_new_device();
    // The first dispatched observer removes its peer, regardless of registration order.
    REQUIRE(first.calls + second.calls == 1);
    Observer &remaining = first.calls ? first : second;
    REQUIRE(remaining.result == ESP_OK);
    fixture.notify_new_device();
    REQUIRE(remaining.calls == 2);
    REQUIRE(ESP_OK == cdc_host_common_unregister_dev_event_cb(remaining.handle));
}

TEST_CASE("Common terminal event callbacks close with other polls still pending", "[common][lifecycle][fault]")
{
    CommonFixture fixture;
    REQUIRE(ESP_OK == fixture.open(64));
    unsigned submits = fixture.submits;
    SECTION("BULK IN stall") {
        fixture.close_from_error = true;
        fixture.complete(0x82, USB_TRANSFER_STATUS_STALL, 0);
        REQUIRE(fixture.errors == 1);
        REQUIRE(fixture.last_error == USB_TRANSFER_STATUS_STALL);
    }
    SECTION("notification transfer error") {
        fixture.close_from_error = true;
        fixture.complete(0x81, USB_TRANSFER_STATUS_ERROR, 0);
        REQUIRE(fixture.errors == 1);
        REQUIRE(fixture.last_error == USB_TRANSFER_STATUS_ERROR);
    }
    SECTION("device disconnected") {
        fixture.close_from_disconnect = true;
        fixture.disconnect_pending = true;
        usb_host_client_unblock_mock_callback(fixture.client, 0);
        REQUIRE(xEventGroupWaitBits(fixture.events, BIT1, pdTRUE, pdTRUE, pdMS_TO_TICKS(1000)) & BIT1);
    }
    REQUIRE(fixture.callback_close_result == ESP_OK);
    REQUIRE(fixture.port == nullptr);
    REQUIRE(ESP_OK == cdc_host_common_release(fixture.driver));
    fixture.driver = nullptr;
    REQUIRE(fixture.submits == submits);
    REQUIRE(fixture.data_callbacks == 0);
    REQUIRE(fixture.safe_cleanup);
    REQUIRE(fixture.inflight.empty());
    REQUIRE(fixture.allocated.empty());
    REQUIRE(fixture.claimed.empty());
}

TEST_CASE("Common open synchronizes endpoint toggles before submitting polls", "[common][toggle]")
{
    CommonFixture fixture;
    SECTION("all claimed endpoints") {
        REQUIRE(ESP_OK == fixture.open());
        REQUIRE(fixture.reset_endpoints == std::vector<uint8_t> {0x02, 0x82, 0x81});
    }
    SECTION("notification interface is not claimed when disabled") {
        REQUIRE(ESP_OK == fixture.open(0, 0, CDC_HOST_COMMON_OPEN_FLAG_DISABLE_NOTIFICATION));
        REQUIRE(fixture.reset_endpoints == std::vector<uint8_t> {0x02, 0x82});
        REQUIRE(fixture.claimed == std::set<uint8_t> {1});
    }
    SECTION("bulk endpoints are synchronized without allocated data transfers") {
        REQUIRE(ESP_OK == fixture.open(0, 0, 0, 0, 0));
        REQUIRE(fixture.reset_endpoints == std::vector<uint8_t> {0x02, 0x82, 0x81});
        REQUIRE(fixture.submits == 1);
    }
    REQUIRE(fixture.reset_requests_valid);
    REQUIRE(fixture.polls_after_reset);
    REQUIRE(fixture.completed_resets == fixture.expected_resets);
    fixture.close();
    REQUIRE(ESP_OK == fixture.open());
    REQUIRE(fixture.reset_endpoints == std::vector<uint8_t> {0x02, 0x82, 0x81});
    REQUIRE(fixture.polls_after_reset);
    fixture.close();
}

TEST_CASE("Common endpoint synchronization failures roll back before polling and allow retry", "[common][toggle][fault]")
{
    CommonFixture fixture;
    esp_err_t expected = ESP_FAIL;
    SECTION("OUT request submission fails") {
        fixture.fail_reset_submit = 1;
    }
    SECTION("IN request submission fails") {
        fixture.fail_reset_submit = 2;
    }
    SECTION("notification request submission fails") {
        fixture.fail_reset_submit = 3;
    }
    SECTION("OUT request stalls") {
        fixture.fail_reset_completion = 1;
        expected = ESP_ERR_INVALID_RESPONSE;
    }
    SECTION("IN request stalls") {
        fixture.fail_reset_completion = 2;
        expected = ESP_ERR_INVALID_RESPONSE;
    }
    SECTION("notification request stalls") {
        fixture.fail_reset_completion = 3;
        expected = ESP_ERR_INVALID_RESPONSE;
    }
    REQUIRE(expected == fixture.open());
    REQUIRE(fixture.port == nullptr);
    REQUIRE(fixture.submits == 0);
    REQUIRE(fixture.reset_requests_valid);
    REQUIRE(fixture.allocated.empty());
    REQUIRE(fixture.inflight.empty());
    REQUIRE(fixture.claimed.empty());
    REQUIRE(fixture.device_opens == fixture.device_closes);
    REQUIRE(fixture.safe_cleanup);
    fixture.fail_reset_submit = 0;
    fixture.fail_reset_completion = 0;
    REQUIRE(ESP_OK == fixture.open());
    REQUIRE(fixture.polls_after_reset);
    fixture.close();
}

TEST_CASE("Common timed out endpoint synchronization retains storage until late completion", "[common][toggle][fault]")
{
    CommonFixture fixture;
    SECTION("OUT request") {
        fixture.timeout_reset = 1;
    }
    SECTION("IN request") {
        fixture.timeout_reset = 2;
    }
    SECTION("notification request") {
        fixture.timeout_reset = 3;
    }
    REQUIRE(ESP_ERR_TIMEOUT == fixture.open());
    REQUIRE(fixture.port == nullptr);
    REQUIRE(fixture.submits == 0);
    REQUIRE(fixture.frees == 0);
    REQUIRE(fixture.inflight.size() == 1);
    fixture.complete(0, USB_TRANSFER_STATUS_COMPLETED, USB_SETUP_PACKET_SIZE);
    REQUIRE(fixture.allocated.empty());
    REQUIRE(fixture.inflight.empty());
    REQUIRE(fixture.claimed.empty());
    REQUIRE(fixture.safe_cleanup);
    fixture.timeout_reset = 0;
    REQUIRE(ESP_OK == fixture.open());
    fixture.close();
}

TEST_CASE("Common disconnect during endpoint synchronization never exposes a port", "[common][toggle][fault]")
{
    CommonFixture fixture;
    SECTION("OUT request") {
        fixture.disconnect_reset = 1;
    }
    SECTION("IN request") {
        fixture.disconnect_reset = 2;
    }
    SECTION("notification request") {
        fixture.disconnect_reset = 3;
    }
    REQUIRE(ESP_OK != fixture.open());
    REQUIRE(fixture.port == nullptr);
    REQUIRE(fixture.submits == 0);
    REQUIRE(fixture.port_events == 0);
    REQUIRE(fixture.frees == 0);
    fixture.complete(0, USB_TRANSFER_STATUS_NO_DEVICE, 0);
    REQUIRE(fixture.allocated.empty());
    REQUIRE(fixture.inflight.empty());
    REQUIRE(fixture.claimed.empty());
    REQUIRE(fixture.safe_cleanup);
}

#ifdef CDC_HOST_COMMON_SUSPEND_RESUME_API_SUPPORTED
TEST_CASE("Common resume during opening waits for endpoint synchronization", "[common][toggle]")
{
    CommonFixture fixture;
    SECTION("OUT synchronization") {
        fixture.resume_reset = 1;
    }
    SECTION("IN synchronization") {
        fixture.resume_reset = 2;
    }
    SECTION("notification synchronization") {
        fixture.resume_reset = 3;
    }
    REQUIRE(ESP_OK == fixture.open());
    REQUIRE(fixture.completed_resets == 3);
    REQUIRE(fixture.polls_after_reset);
    REQUIRE(fixture.submits == 2);
    REQUIRE(fixture.port_events == 0);
    fixture.close();
}
#endif

TEST_CASE("Common open from the device callback is rejected without blocking", "[common][toggle]")
{
    CommonFixture fixture;
    cdc_host_common_dev_event_cb_handle_t callback = nullptr;
    esp_err_t result = ESP_FAIL;
    REQUIRE(ESP_OK == cdc_host_common_register_dev_event_cb(fixture.driver, [](const cdc_host_common_dev_event_data_t *, void *arg) {
        auto *result = static_cast<esp_err_t *>(arg);
        *result = CommonFixture::current->open();
    }, &result, &callback));
    fixture.notify_new_device();
    REQUIRE(result == ESP_ERR_INVALID_STATE);
    REQUIRE(fixture.port == nullptr);
    REQUIRE(fixture.allocated.empty());
    REQUIRE(fixture.reset_endpoints.empty());
    REQUIRE(ESP_OK == cdc_host_common_unregister_dev_event_cb(callback));
    REQUIRE(ESP_OK == fixture.open());
    fixture.close();
}

TEST_CASE("Common disabled notification still synchronizes its shared interface endpoint", "[common][toggle]")
{
    const uint8_t descriptors[] = {
        9, 2, 44, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 3, 0xFF, 0, 0, 0,
        5, 0x24, 0, 0x10, 1,
        7, 5, 0x82, 2, 64, 0, 0,
        7, 5, 0x02, 2, 64, 0, 0,
        7, 5, 0x81, 3, 8, 0, 10,
    };
    CommonFixture fixture(descriptors);
    fixture.shared_notification = true;
    REQUIRE(ESP_OK == fixture.open(0, 0, CDC_HOST_COMMON_OPEN_FLAG_DISABLE_NOTIFICATION));
    REQUIRE(fixture.reset_endpoints == std::vector<uint8_t> {0x82, 0x02, 0x81});
    REQUIRE(fixture.claimed == std::set<uint8_t> {0});
    REQUIRE(fixture.allocated.size() == 3);
    REQUIRE(fixture.submits == 1);
    REQUIRE(fixture.reset_requests_valid);
    REQUIRE(fixture.polls_after_reset);
    fixture.close();
}
