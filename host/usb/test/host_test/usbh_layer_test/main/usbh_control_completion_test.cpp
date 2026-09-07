/*
 * SPDX-FileCopyrightText: 2026 bigtreetech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <catch2/catch_test_macros.hpp>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usbh.h"

extern "C" {
#include "Mockhcd.h"
}

namespace {

class ControlCompletionFixture {
public:
    ControlCompletionFixture()
    {
        active = this;
        Mockhcd_Init();
        hcd_pipe_alloc_Stub(pipe_alloc);
        hcd_pipe_free_IgnoreAndReturn(ESP_OK);
        hcd_pipe_get_mps_IgnoreAndReturn(64);
        hcd_pipe_command_IgnoreAndReturn(ESP_OK);
        hcd_urb_enqueue_Stub(enqueue);
        hcd_urb_dequeue_Stub(dequeue);

        usbh_config_t config = {};
        config.proc_req_cb = [](usb_proc_req_source_t, bool, void *) {
            return false;
        };
        config.event_cb = event_callback;
        REQUIRE(ESP_OK == usbh_install(&config));

        usbh_dev_params_t params = {};
        params.uid = 1;
        params.speed = USB_SPEED_FULL;
        params.root_port_hdl = reinterpret_cast<hcd_port_handle_t>(this);
        REQUIRE(ESP_OK == usbh_devs_add(&params));
        REQUIRE(ESP_OK == usbh_devs_open_uid(params.uid, &device));

        // The MSC caller can preempt the USB daemon as soon as completion is
        // reported, including when GET_MAX_LUN leads directly to cleanup.
        REQUIRE(pdPASS == xTaskCreate(close_device_task, "ep0_close", 4096, this,
                                      uxTaskPriorityGet(nullptr) + 1, &close_task));
    }

    ~ControlCompletionFixture()
    {
        if (close_task) {
            vTaskDelete(close_task);
        }
        if (device) {
            CHECK(ESP_OK == usbh_dev_close(device));
        }
        CHECK(ESP_ERR_NOT_FINISHED == usbh_devs_mark_all_free());
        CHECK(ESP_OK == usbh_process());
        CHECK(ESP_OK == usbh_uninstall());
        Mockhcd_Verify();
        Mockhcd_Destroy();
        active = nullptr;
    }

    void complete(hcd_pipe_event_t event)
    {
        REQUIRE(queued != nullptr);
        queued->transfer.status = event == HCD_PIPE_EVENT_URB_DONE ? USB_TRANSFER_STATUS_COMPLETED :
                                  event == HCD_PIPE_EVENT_ERROR_STALL ? USB_TRANSFER_STATUS_STALL : USB_TRANSFER_STATUS_ERROR;
        completed = queued;
        queued = nullptr;
        REQUIRE_FALSE(pipe_callback(pipe, event, pipe_callback_arg, false));
        REQUIRE(ESP_OK == usbh_process());
    }

    usb_device_handle_t device = nullptr;
    unsigned completions = 0;
    bool resubmit = false;
    esp_err_t resubmit_result = ESP_FAIL;
    esp_err_t close_result = ESP_FAIL;
    bool closed_before_event_return = false;

private:
    static ControlCompletionFixture *active;
    hcd_pipe_handle_t pipe = nullptr;
    hcd_pipe_callback_t pipe_callback = nullptr;
    void *pipe_callback_arg = nullptr;
    TaskHandle_t close_task = nullptr;
    urb_t *queued = nullptr;
    urb_t *completed = nullptr;

    static esp_err_t pipe_alloc(hcd_port_handle_t, const hcd_pipe_config_t *config,
                                hcd_pipe_handle_t *handle, int)
    {
        active->pipe = reinterpret_cast<hcd_pipe_handle_t>(active);
        active->pipe_callback = config->callback;
        active->pipe_callback_arg = config->callback_arg;
        *handle = active->pipe;
        return ESP_OK;
    }

    static esp_err_t enqueue(hcd_pipe_handle_t, urb_t *urb, int)
    {
        REQUIRE(active->queued == nullptr);
        active->queued = urb;
        return ESP_OK;
    }

    static urb_t *dequeue(hcd_pipe_handle_t, int)
    {
        urb_t *urb = active->completed;
        active->completed = nullptr;
        return urb;
    }

    static void event_callback(usbh_event_data_t *event, void *)
    {
        if (event->event != USBH_EVENT_CTRL_XFER) {
            return;
        }
        auto &fixture = *active;
        ++fixture.completions;
        if (fixture.resubmit && fixture.completions == 1) {
            fixture.resubmit_result = usbh_dev_submit_ctrl_urb(fixture.device, event->ctrl_xfer_data.urb);
        } else {
            xTaskNotifyGive(fixture.close_task);
            fixture.closed_before_event_return = fixture.close_result == ESP_OK;
        }
    }

    static void close_device_task(void *arg)
    {
        auto &fixture = *static_cast<ControlCompletionFixture *>(arg);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        fixture.close_result = usbh_dev_close(fixture.device);
        if (fixture.close_result == ESP_OK) {
            fixture.device = nullptr;
        }
        fixture.close_task = nullptr;
        vTaskDelete(nullptr);
    }
};

ControlCompletionFixture *ControlCompletionFixture::active = nullptr;

} // namespace

TEST_CASE_METHOD(ControlCompletionFixture, "USBH retires EP0 before a completion can close the device", "[usbh][control]")
{
    uint8_t data[USB_SETUP_PACKET_SIZE + 1] = {};
    auto *setup = reinterpret_cast<usb_setup_packet_t *>(data);
    setup->bmRequestType = 0xa1;
    setup->bRequest = 0xfe; // GET_MAX_LUN
    setup->wIndex = 3;
    setup->wLength = 1;
    urb_t urb = {
        .tailq_entry = {},
        .hcd_ptr = nullptr,
        .hcd_var = 0,
        .usb_host_client = nullptr,
        .usb_host_inflight = false,
        .transfer = {
            .data_buffer = data,
            .data_buffer_size = sizeof(data),
            .num_bytes = sizeof(data),
            .actual_num_bytes = 0,
            .flags = 0,
            .device_handle = device,
            .bEndpointAddress = 0,
            .status = USB_TRANSFER_STATUS_COMPLETED,
            .timeout_ms = 0,
            .callback = [](usb_transfer_t *) {},
            .context = nullptr,
            .num_isoc_packets = 0,
        },
    };
    hcd_pipe_event_t event = HCD_PIPE_EVENT_URB_DONE;
    SECTION("Successful control transfer") {}
    SECTION("Control STALL") {
        event = HCD_PIPE_EVENT_ERROR_STALL;
    }
    SECTION("Control transfer error") {
        event = HCD_PIPE_EVENT_ERROR_XFER;
    }
    SECTION("Completion resubmits the same URB before closing") {
        resubmit = true;
    }

    REQUIRE(ESP_OK == usbh_dev_submit_ctrl_urb(device, &urb));
    complete(event);
    if (resubmit) {
        REQUIRE(ESP_OK == resubmit_result);
        REQUIRE(device != nullptr);
        complete(HCD_PIPE_EVENT_URB_DONE);
    }
    CHECK(completions == (resubmit ? 2 : 1));
    CHECK(close_result == ESP_OK);
    CHECK(closed_before_event_return);
}
