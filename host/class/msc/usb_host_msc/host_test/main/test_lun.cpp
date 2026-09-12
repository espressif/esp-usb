/*
 * SPDX-FileCopyrightText: 2026 bigtreetech
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstring>
#include <catch2/catch_test_macros.hpp>

#include "usb/msc_host.h"
#include "esp_private/msc_scsi_bot.h"
#include "mock_add_usb_device.h"
#include "freertos/task.h"

extern "C" {
#include "Mockusb_host.h"
}

namespace {

enum : uint8_t { TEST_UNIT_READY = 0x00, REQUEST_SENSE = 0x03, INQUIRY = 0x12,
                 READ_CAPACITY = 0x25, READ10 = 0x28, WRITE10 = 0x2a
               };

enum class CswFault { NONE, SIGNATURE, TAG, RESERVED_STATUS, PHASE_ERROR, SHORT_RESPONSE, RESIDUE };

const usb_device_desc_t device_descriptor = {
    .bLength = sizeof(usb_device_desc_t),
    .bDescriptorType = USB_B_DESCRIPTOR_TYPE_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64,
    .idVendor = 0,
    .idProduct = 0,
    .bcdDevice = 0,
    .iManufacturer = 0,
    .iProduct = 0,
    .iSerialNumber = 0,
    .bNumConfigurations = 1,
};

const struct __attribute__((packed))
{
    usb_config_desc_t config;
    usb_intf_desc_t interface;
    usb_ep_desc_t in, out;
} descriptors = {
    .config = {
        .bLength = 9, .bDescriptorType = 2, .wTotalLength = 32, .bNumInterfaces = 1,
        .bConfigurationValue = 1, .iConfiguration = 0, .bmAttributes = 0x80, .bMaxPower = 50
    },
    .interface = {
        .bLength = 9, .bDescriptorType = 4, .bInterfaceNumber = 3, .bAlternateSetting = 0,
        .bNumEndpoints = 2, .bInterfaceClass = 8, .bInterfaceSubClass = 6, .bInterfaceProtocol = 0x50, .iInterface = 0
    },
    .in = { .bLength = 7, .bDescriptorType = 5, .bEndpointAddress = 0x81, .bmAttributes = 2, .wMaxPacketSize = 64, .bInterval = 0 },
    .out = { .bLength = 7, .bDescriptorType = 5, .bEndpointAddress = 0x02, .bmAttributes = 2, .wMaxPacketSize = 64, .bInterval = 0 },
};

// Only BOT responses are supplied here; use the existing USB mock for device
// ownership, transfer allocation and the actual MSC driver for all commands.
class LunFixture {
public:
    LunFixture()
    {
        active = this;
        Mockusb_host_Init();
        usb_host_mock_dev_list_init();
        REQUIRE(ESP_OK == usb_host_mock_add_device(1, &device_descriptor, &descriptors.config, USB_SPEED_FULL));
        usb_host_client_register_Stub(register_callback);
        usb_host_client_deregister_Stub(usb_host_client_deregister_mock_callback);
        usb_host_device_open_Stub(open_callback);
        usb_host_device_close_Stub(close_callback);
        usb_host_get_active_config_descriptor_Stub(usb_host_get_active_config_descriptor_mock_callback);
        usb_host_interface_claim_Stub(claim_callback);
        usb_host_interface_release_Stub(release_callback);
        usb_host_transfer_alloc_Stub(alloc_callback);
        usb_host_transfer_free_Stub(free_callback);
        usb_host_endpoint_halt_IgnoreAndReturn(ESP_OK);
        usb_host_endpoint_flush_IgnoreAndReturn(ESP_OK);
        usb_host_endpoint_clear_Stub(endpoint_clear_callback);
        usb_host_transfer_submit_Stub(bulk_callback);
        usb_host_transfer_submit_control_Stub(control_callback);

        msc_host_driver_config_t config = {};
        config.callback = [](const msc_host_event_t *, void *arg) {
            ++static_cast<LunFixture *>(arg)->public_events;
        };
        config.callback_arg = this;
        REQUIRE(ESP_OK == msc_host_install(&config));
        for (auto &size : sector_sizes) {
            size = 512;
        }
    }

    ~LunFixture()
    {
        if (device) {
            CHECK(ESP_OK == msc_host_uninstall_device(device));
        }
        CHECK(ESP_OK == msc_host_uninstall());
        check_closed();
        Mockusb_host_Verify();
        Mockusb_host_Destroy();
        active = nullptr;
    }

    msc_host_device_handle_t device = nullptr;
    uint8_t max_lun = 1;
    uint16_t ready_luns = 1 << 1;
    uint16_t inquiry_fail_luns = 0, tur_fail_luns = 0, capacity_fail_luns = 0;
    uint32_t sector_sizes[16] = {};
    uint8_t delayed_lun = 0xff;
    uint16_t pending_luns = 0;
    uint8_t pending_sense_key = 0x02, empty_sense_ascq = 0;
    int transport_error_lun = -1;
    uint8_t transport_error_opcode = TEST_UNIT_READY;
    CswFault csw_fault = CswFault::NONE;
    int fixed_lun = -1;
    bool fail_read = false;
    usb_transfer_status_t get_max_status = USB_TRANSFER_STATUS_COMPLETED;
    usb_transfer_status_t reset_status = USB_TRANSFER_STATUS_COMPLETED;
    int get_max_length = USB_SETUP_PACKET_SIZE + 1;
    unsigned get_max_count = 0, reset_count = 0, sense_count = 0;
    unsigned ready_count[16] = {}, inquiry_count[16] = {}, capacity_count[16] = {};
    unsigned open_calls = 0, open_devices = 0, claims = 0, transfers = 0;
    esp_err_t claim_result = ESP_OK;
    unsigned release_calls = 0, public_events = 0, probe_lifetime_checks = 0;
    bool release_pending_once = false;
    bool inject_probe_events = false;
    // Packet-parity contract for these 64-byte endpoints, not a USB bus simulator.
    uint8_t host_pid[2] = {}, device_pid[2] = {}; // OUT, IN
    unsigned bulk_count = 0, clear_count = 0;
    struct {
        uint8_t endpoint;
        unsigned bulk_count;
    } clears[8] = {};
    uint8_t clear_fail_endpoint = 0;

    void check_closed() const
    {
        CHECK(open_devices == 0);
        CHECK(claims == 0);
        CHECK(transfers == 0);
    }

private:
    static LunFixture *active;
    enum { CBW, DATA, CSW, RESET_REQUIRED } phase = CBW;
    uint32_t tag = 0, data_length = 0;
    uint8_t opcode = 0, lun = 0, status = 0;
    uint8_t sense_key = 0, sense_asc = 0, sense_ascq = 0;
    usb_host_client_event_cb_t client_event_callback = nullptr;
    void *client_event_arg = nullptr;

    void disconnect(usb_device_handle_t handle)
    {
        usb_host_client_event_msg_t event = {};
        event.event = USB_HOST_CLIENT_EVENT_DEV_GONE;
        event.dev_gone.dev_hdl = handle;
        client_event_callback(&event, client_event_arg);
    }

    static esp_err_t register_callback(const usb_host_client_config_t *config,
                                       usb_host_client_handle_t *handle, int count)
    {
        active->client_event_callback = config->async.client_event_callback;
        active->client_event_arg = config->async.callback_arg;
        return usb_host_client_register_mock_callback(config, handle, count);
    }

    static esp_err_t open_callback(usb_host_client_handle_t client, uint8_t address,
                                   usb_device_handle_t *handle, int count)
    {
        ++active->open_calls;
        // The shared mock omits this real USB Host rule: the same client
        // cannot open the same device twice. This fixture has one device.
        if (active->open_devices > 0) {
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t err = usb_host_device_open_mock_callback(client, address, handle, count);
        if (err == ESP_OK) {
            ++active->open_devices;
        }
        return err;
    }

    static esp_err_t close_callback(usb_host_client_handle_t client, usb_device_handle_t handle, int count)
    {
        if (active->claims > 0) {
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t err = usb_host_device_close_mock_callback(client, handle, count);
        if (err == ESP_OK) {
            REQUIRE(active->open_devices > 0);
            --active->open_devices;
        }
        return err;
    }

    static esp_err_t claim_callback(usb_host_client_handle_t, usb_device_handle_t, uint8_t interface,
                                    uint8_t alternate, int)
    {
        CHECK(interface == 3);
        CHECK(alternate == 0);
        CHECK(active->claims == 0);
        if (active->claim_result != ESP_OK) {
            return active->claim_result;
        }
        // New host channels start at DATA0; the connected device retains its PID.
        active->host_pid[0] = active->host_pid[1] = 0;
        ++active->claims;
        return ESP_OK;
    }

    static esp_err_t release_callback(usb_host_client_handle_t, usb_device_handle_t, uint8_t interface, int)
    {
        CHECK(interface == 3);
        ++active->release_calls;
        REQUIRE(active->claims > 0);
        if (active->release_pending_once) {
            // The transfer callback can wake the caller before USB Host has
            // finished updating its in-flight transfer count.
            active->release_pending_once = false;
            return ESP_ERR_INVALID_STATE;
        }
        --active->claims;
        return ESP_OK;
    }

    static esp_err_t endpoint_clear_callback(usb_device_handle_t, uint8_t endpoint, int)
    {
        CHECK((endpoint == 0x81 || endpoint == 0x02));
        active->host_pid[(endpoint & 0x80) != 0] = 0;
        return ESP_OK;
    }

    static esp_err_t alloc_callback(size_t size, int packets, usb_transfer_t **transfer, int count)
    {
        esp_err_t err = usb_host_transfer_alloc_mock_callback(size, packets, transfer, count);
        if (err == ESP_OK) {
            ++active->transfers;
        }
        return err;
    }

    static esp_err_t free_callback(usb_transfer_t *transfer, int count)
    {
        if (transfer) {
            REQUIRE(active->transfers > 0);
            --active->transfers;
        }
        return usb_host_transfer_free_mock_callback(transfer, count);
    }

    static esp_err_t control_callback(usb_host_client_handle_t, usb_transfer_t *transfer, int)
    {
        auto &f = *active;
        usb_setup_packet_t setup;
        std::memcpy(&setup, transfer->data_buffer, sizeof(setup));
        REQUIRE(transfer->bEndpointAddress == 0);
        transfer->status = USB_TRANSFER_STATUS_COMPLETED;
        transfer->actual_num_bytes = transfer->num_bytes;
        if (setup.bRequest == 0xfe) {
            CHECK(setup.bmRequestType == 0xa1);
            CHECK(setup.wValue == 0);
            CHECK(setup.wIndex == 3);
            CHECK(setup.wLength == 1);
            CHECK(transfer->num_bytes == USB_SETUP_PACKET_SIZE + 1);
            ++f.get_max_count;
            if (f.inject_probe_events) {
                ++f.probe_lifetime_checks;
                REQUIRE(ESP_ERR_INVALID_STATE == msc_host_uninstall());
                REQUIRE(f.client_event_callback != nullptr);
                f.disconnect(transfer->device_handle);
            }
            transfer->status = f.get_max_status;
            transfer->actual_num_bytes = f.get_max_length;
            transfer->data_buffer[USB_SETUP_PACKET_SIZE] = f.max_lun;
        } else if (setup.bRequest == 0xff) {
            CHECK(setup.bmRequestType == 0x21);
            CHECK(setup.wIndex == 3);
            CHECK(setup.wLength == 0);
            ++f.reset_count;
            transfer->status = f.reset_status;
            if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
                f.phase = CBW;
            }
        } else {
            CHECK(setup.bRequest == USB_B_REQUEST_CLEAR_FEATURE);
            CHECK(setup.bmRequestType == 2);
            CHECK((setup.wIndex == 0x81 || setup.wIndex == 0x02));
            REQUIRE(f.clear_count < sizeof(f.clears) / sizeof(f.clears[0]));
            f.clears[f.clear_count].endpoint = setup.wIndex;
            f.clears[f.clear_count++].bulk_count = f.bulk_count;
            if (setup.wIndex == f.clear_fail_endpoint) {
                transfer->status = USB_TRANSFER_STATUS_ERROR;
            } else {
                f.device_pid[(setup.wIndex & 0x80) != 0] = 0;
            }
        }
        transfer->callback(transfer);
        return ESP_OK;
    }

    static esp_err_t bulk_callback(usb_transfer_t *transfer, int)
    {
        auto &f = *active;
        const unsigned ep = (transfer->bEndpointAddress & 0x80) != 0;
        if (f.host_pid[ep] != f.device_pid[ep] || f.phase == RESET_REQUIRED) {
            // Return through the normal transfer path instead of throwing a
            // Catch assertion through the driver's C stack.
            CHECK(f.host_pid[ep] == f.device_pid[ep]);
            transfer->status = USB_TRANSFER_STATUS_STALL;
            transfer->actual_num_bytes = 0;
            transfer->callback(transfer);
            return ESP_OK;
        }
        ++f.bulk_count;
        uint8_t *data = transfer->data_buffer;
        transfer->status = USB_TRANSFER_STATUS_COMPLETED;
        transfer->actual_num_bytes = transfer->num_bytes;

        if (f.phase == CBW) {
            REQUIRE(transfer->bEndpointAddress == 0x02);
            REQUIRE(transfer->num_bytes == 31);
            CHECK(std::memcmp(data, "USBC", 4) == 0);
            std::memcpy(&f.tag, data + 4, sizeof(f.tag));
            std::memcpy(&f.data_length, data + 8, sizeof(f.data_length));
            f.lun = data[13];
            f.opcode = data[15];
            REQUIRE(f.lun < 16);
            if (f.fixed_lun >= 0) {
                CHECK(f.lun == f.fixed_lun);
            }
            f.status = 0;
            if (f.lun == f.transport_error_lun && f.opcode == f.transport_error_opcode) {
                transfer->status = USB_TRANSFER_STATUS_ERROR;
                transfer->actual_num_bytes = 0;
                transfer->callback(transfer);
                return ESP_OK;
            }
            if (f.opcode == TEST_UNIT_READY) {
                ++f.ready_count[f.lun];
                if (f.lun == f.delayed_lun && f.ready_count[f.lun] > 1) {
                    f.ready_luns |= 1 << f.lun;
                }
                f.status = (f.ready_luns & (1 << f.lun)) ? 0 : 1;
                if (f.status) {
                    f.sense_key = 0x02;
                    f.sense_asc = 0x3a; // NOT READY, MEDIUM NOT PRESENT
                    f.sense_ascq = f.empty_sense_ascq;
                    if (f.lun == f.delayed_lun || (f.pending_luns & (1 << f.lun))) {
                        f.sense_key = f.pending_sense_key;
                        // Becoming ready or NOT READY TO READY TRANSITION.
                        f.sense_asc = f.pending_sense_key == 0x06 ? 0x28 : 0x04;
                        f.sense_ascq = f.pending_sense_key == 0x06 ? 0x00 : 0x01;
                    }
                }
                if (f.tur_fail_luns & (1 << f.lun)) {
                    f.status = 1;
                    f.sense_key = 0x03;
                    f.sense_asc = 0x11;
                }
            } else if (f.opcode == INQUIRY) {
                ++f.inquiry_count[f.lun];
                f.status = (f.inquiry_fail_luns & (1 << f.lun)) ? 1 : 0;
            } else if (f.opcode == READ_CAPACITY) {
                ++f.capacity_count[f.lun];
                f.status = (f.capacity_fail_luns & (1 << f.lun)) ? 1 : 0;
            } else if (f.opcode == READ10 && f.fail_read) {
                f.fail_read = false;
                f.status = 1;
                f.sense_key = 0x03;
                f.sense_asc = 0x11; // MEDIUM ERROR, UNRECOVERED READ ERROR
            }
            f.phase = f.data_length ? DATA : CSW;
        } else if (f.phase == DATA) {
            REQUIRE(f.data_length <= transfer->data_buffer_size);
            if (f.opcode != WRITE10) {
                REQUIRE(transfer->bEndpointAddress == 0x81);
                std::memset(data, 0, f.data_length);
            }
            switch (f.opcode) {
            case READ_CAPACITY:
                REQUIRE(f.data_length == 8);
                CHECK((f.ready_luns & (1 << f.lun)) != 0);
                data[3] = 63; // Last LBA, big endian
                for (unsigned i = 0; i < 4; ++i) {
                    data[4 + i] = f.sector_sizes[f.lun] >> (8 * (3 - i));
                }
                break;
            case REQUEST_SENSE:
                REQUIRE(f.data_length == 18);
                ++f.sense_count;
                data[0] = 0x70;
                data[2] = f.sense_key;
                data[7] = 10;
                data[12] = f.sense_asc;
                data[13] = f.sense_ascq;
                f.sense_key = f.sense_asc = f.sense_ascq = 0;
                break;
            case READ10:
                std::memset(data, 0xa5, f.data_length);
                break;
            case WRITE10:
                CHECK(transfer->bEndpointAddress == 0x02);
                break;
            case INQUIRY:
                break;
            default:
                FAIL("Unexpected SCSI data phase");
            }
            transfer->actual_num_bytes = f.data_length;
            f.phase = CSW;
        } else {
            REQUIRE(transfer->bEndpointAddress == 0x81);
            std::memcpy(data, "USBS", 4);
            std::memcpy(data + 4, &f.tag, sizeof(f.tag));
            std::memset(data + 8, 0, 4);
            data[12] = f.status;
            transfer->actual_num_bytes = 13;
            f.phase = CBW;
            if (f.lun == 1 && f.opcode == TEST_UNIT_READY) {
                switch (f.csw_fault) {
                case CswFault::SIGNATURE:
                    data[0] ^= 1;
                    break;
                case CswFault::TAG:
                    data[4] ^= 1;
                    break;
                case CswFault::RESERVED_STATUS:
                    data[12] = 3;
                    break;
                case CswFault::PHASE_ERROR:
                    data[12] = 2;
                    break;
                case CswFault::SHORT_RESPONSE:
                    transfer->actual_num_bytes = 12;
                    break;
                case CswFault::RESIDUE:
                    data[8] = 1;
                    break;
                case CswFault::NONE:
                    break;
                }
                if (f.csw_fault != CswFault::NONE) {
                    f.phase = RESET_REQUIRED;
                }
            }
        }
        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
            const unsigned parity = (static_cast<unsigned>(transfer->actual_num_bytes) + 63) / 64 % 2;
            f.host_pid[ep] ^= parity;
            f.device_pid[ep] ^= parity;
        }
        transfer->callback(transfer);
        return ESP_OK;
    }
};

LunFixture *LunFixture::active = nullptr;

} // namespace

TEST_CASE_METHOD(LunFixture, "MSC probes an empty slot and leaves LUN selection to the caller", "[msc][lun]")
{
    msc_host_lun_info_t info = {};
    const TickType_t started = xTaskGetTickCount();
    REQUIRE(ESP_OK == msc_host_probe_luns(1, 5000, &info));
    CHECK(xTaskGetTickCount() - started < pdMS_TO_TICKS(2500));
    CHECK(info.max_lun == 1);
    REQUIRE(info.ready_lun_mask == 2);
    CHECK(info.failed_lun_mask == 0);
    CHECK(get_max_count == 1);
    CHECK(ready_count[0] == 1);
    CHECK(ready_count[1] == 1);
    CHECK(sense_count == 1);
    check_closed();

    // The application accepts the sole candidate and explicitly installs it.
    const uint8_t selected_lun = __builtin_ctz(static_cast<unsigned>(info.ready_lun_mask));
    fixed_lun = selected_lun;
    REQUIRE(ESP_OK == msc_host_install_device_lun(1, selected_lun, &device));
    uint8_t sector[512] = {};
    REQUIRE(ESP_OK == scsi_cmd_read10(device, sector, 0, 1, sizeof(sector)));
    CHECK(sector[0] == 0xa5);
    REQUIRE(ESP_OK == scsi_cmd_write10(device, sector, 0, 1, sizeof(sector)));

    const unsigned previous_sense_count = sense_count;
    fail_read = true;
    CHECK(ESP_FAIL == scsi_cmd_read10(device, sector, 0, 1, sizeof(sector)));
    CHECK(sense_count > previous_sense_count);

    // Even if the formerly empty slot becomes ready, recovery must retain the
    // mounted slot and must not issue GET_MAX_LUN or probe LUN 0 again.
    ready_luns |= 1;
    const unsigned previous_lun0_count = ready_count[0];
    const unsigned previous_reset_count = reset_count;
    REQUIRE(ESP_OK == msc_host_reset_recovery(device));
    CHECK(reset_count == previous_reset_count + 1);
    CHECK(get_max_count == 2);
    CHECK(ready_count[0] == previous_lun0_count);
    REQUIRE(ESP_OK == scsi_cmd_read10(device, sector, 0, 1, sizeof(sector)));
}

TEST_CASE_METHOD(LunFixture, "MSC reports both ready LUNs without choosing one", "[msc][lun]")
{
    ready_luns = 3;
    msc_host_lun_info_t info = {};
    REQUIRE(ESP_OK == msc_host_probe_luns(1, 0, &info));
    CHECK(info.max_lun == 1);
    REQUIRE(info.ready_lun_mask == 3);
    CHECK(info.failed_lun_mask == 0);
    CHECK(capacity_count[0] == 1);
    CHECK(capacity_count[1] == 1);
    check_closed();

    uint8_t selected_lun = 0;
    SECTION("Application policy selects LUN one") {
        selected_lun = 1;
    }
    SECTION("Application accepts any candidate and chooses the lowest set bit") {
        selected_lun = __builtin_ctz(static_cast<unsigned>(info.ready_lun_mask));
        CHECK(selected_lun == 0);
    }
    fixed_lun = selected_lun;
    REQUIRE(ESP_OK == msc_host_install_device_lun(1, selected_lun, &device));
    CHECK(ready_count[selected_lun] == 2);
    CHECK(ready_count[1 - selected_lun] == 1);
}

TEST_CASE_METHOD(LunFixture, "MSC legacy install keeps LUN zero without GET_MAX_LUN", "[msc][lun]")
{
    ready_luns = 3;
    fixed_lun = 0;
    get_max_status = USB_TRANSFER_STATUS_NO_DEVICE; // Must never be requested.
    REQUIRE(ESP_OK == msc_host_install_device(1, &device));
    CHECK(get_max_count == 0);
    CHECK(ready_count[0] == 1);
    CHECK(ready_count[1] == 0);
}

TEST_CASE_METHOD(LunFixture, "MSC installs a configured LUN without preliminary discovery", "[msc][lun]")
{
    ready_luns = 3;
    fixed_lun = 1;
    REQUIRE(ESP_OK == msc_host_install_device_lun(1, 1, &device));
    CHECK(get_max_count == 1);
    CHECK(inquiry_count[0] == 0);
    CHECK(ready_count[0] == 0);
    CHECK(capacity_count[0] == 0);
    CHECK(ready_count[1] == 1);
    CHECK(capacity_count[1] == 1);
}

TEST_CASE_METHOD(LunFixture, "MSC can select another LUN after uninstalling", "[msc][lun]")
{
    ready_luns = 3;
    fixed_lun = 0;
    REQUIRE(ESP_OK == msc_host_install_device_lun(1, 0, &device));
    REQUIRE(ESP_OK == msc_host_uninstall_device(device));
    device = nullptr;
    check_closed();

    fixed_lun = 1;
    REQUIRE(ESP_OK == msc_host_install_device_lun(1, 1, &device));
    CHECK(ready_count[0] == 1);
    CHECK(ready_count[1] == 1);
}

TEST_CASE_METHOD(LunFixture, "MSC probes and explicitly installs LUN fifteen", "[msc][lun]")
{
    max_lun = 15;
    ready_luns = 0x8000;
    msc_host_lun_info_t info = {};
    REQUIRE(ESP_OK == msc_host_probe_luns(1, 0, &info));
    CHECK(info.max_lun == 15);
    CHECK(info.ready_lun_mask == 0x8000);
    CHECK(info.failed_lun_mask == 0);
    CHECK(sense_count == 15);
    check_closed();

    fixed_lun = 15;
    REQUIRE(ESP_OK == msc_host_install_device_lun(1, 15, &device));
    CHECK(ready_count[15] == 2);
    for (unsigned i = 0; i < 15; ++i) {
        CHECK(ready_count[i] == 1);
    }
}

TEST_CASE_METHOD(LunFixture, "MSC probe accepts single-LUN GET_MAX_LUN STALL", "[msc][lun]")
{
    get_max_status = USB_TRANSFER_STATUS_STALL;
    ready_luns = 1;
    fixed_lun = 0;
    msc_host_lun_info_t info = {};
    REQUIRE(ESP_OK == msc_host_probe_luns(1, 0, &info));
    CHECK(info.max_lun == 0);
    CHECK(info.ready_lun_mask == 1);
    CHECK(info.failed_lun_mask == 0);
    CHECK(get_max_count == 1);
    CHECK(ready_count[0] == 1);
    CHECK(ready_count[1] == 0);
    check_closed();
}

TEST_CASE_METHOD(LunFixture, "MSC rejects invalid GET_MAX_LUN responses", "[msc][lun]")
{
    esp_err_t expected = ESP_OK;
    SECTION("Short response") {
        get_max_length = USB_SETUP_PACKET_SIZE;
        expected = ESP_ERR_INVALID_SIZE;
    }
    SECTION("Reserved LUN bits") {
        max_lun = 16;
        expected = ESP_ERR_INVALID_RESPONSE;
    }
    SECTION("Transport error is not a single-LUN STALL") {
        get_max_status = USB_TRANSFER_STATUS_NO_DEVICE;
        expected = ESP_ERR_MSC_INTERNAL;
    }
    msc_host_lun_info_t info = { .ready_lun_mask = 0xffff, .failed_lun_mask = 0xffff, .max_lun = 15 };
    CHECK(expected == msc_host_probe_luns(1, 0, &info));
    CHECK(info.max_lun == 0);
    CHECK(info.ready_lun_mask == 0);
    CHECK(info.failed_lun_mask == 0);
    check_closed();
    CHECK(expected == msc_host_install_device_lun(1, 1, &device));
    CHECK(device == nullptr);
    CHECK(get_max_count == 2);
    CHECK(ready_count[0] == 0);
    CHECK(ready_count[1] == 0);
    check_closed();
}

TEST_CASE_METHOD(LunFixture, "MSC does not fall back from an explicit unavailable LUN", "[msc][lun]")
{
    ready_luns = 1;
    SECTION("LUN exceeds GET_MAX_LUN") {
        CHECK(ESP_ERR_NOT_FOUND == msc_host_install_device_lun(1, 2, &device));
        CHECK(get_max_count == 1);
        CHECK(ready_count[0] == 0);
        CHECK(ready_count[1] == 0);
    }
    SECTION("GET_MAX_LUN STALL only permits LUN zero") {
        get_max_status = USB_TRANSFER_STATUS_STALL;
        CHECK(ESP_ERR_NOT_FOUND == msc_host_install_device_lun(1, 1, &device));
        CHECK(get_max_count == 1);
        CHECK(ready_count[0] == 0);
        CHECK(ready_count[1] == 0);
    }
    SECTION("Specified LUN fails SCSI readiness") {
        tur_fail_luns = 2;
        fixed_lun = 1;
        CHECK(ESP_OK != msc_host_install_device_lun(1, 1, &device));
        CHECK(ready_count[0] == 0);
        CHECK(ready_count[1] == 1);
        CHECK(sense_count == 1);
    }
    SECTION("Specified LUN has unsupported sector geometry") {
        ready_luns = 3;
        sector_sizes[1] = 768;
        fixed_lun = 1;
        CHECK(ESP_ERR_INVALID_SIZE == msc_host_install_device_lun(1, 1, &device));
        CHECK(ready_count[0] == 0);
        CHECK(ready_count[1] == 1);
    }
    CHECK(device == nullptr);
    check_closed();
}

TEST_CASE_METHOD(LunFixture, "MSC validates probe and install parameters before opening a device", "[msc][lun]")
{
    CHECK(ESP_ERR_INVALID_ARG == msc_host_install_device_lun(1, 16, &device));
    CHECK(ESP_ERR_INVALID_ARG == msc_host_install_device_lun(1, 0, nullptr));
    CHECK(ESP_ERR_INVALID_ARG == msc_host_probe_luns(1, 0, nullptr));
    CHECK(open_calls == 0);
    CHECK(get_max_count == 0);
    check_closed();
}

TEST_CASE_METHOD(LunFixture, "MSC rejects competing operations without disturbing an installed LUN", "[msc][lun]")
{
    fixed_lun = 1;
    REQUIRE(ESP_OK == msc_host_install_device_lun(1, 1, &device));
    const unsigned previous_open_calls = open_calls;
    msc_host_device_handle_t other = nullptr;
    msc_host_lun_info_t info = { .ready_lun_mask = 0xffff, .failed_lun_mask = 0xffff, .max_lun = 15 };
    CHECK(ESP_ERR_INVALID_STATE == msc_host_probe_luns(1, 0, &info));
    CHECK(info.max_lun == 0);
    CHECK(info.ready_lun_mask == 0);
    CHECK(info.failed_lun_mask == 0);
    CHECK(ESP_ERR_INVALID_STATE == msc_host_install_device_lun(1, 0, &other));
    CHECK(ESP_ERR_INVALID_STATE == msc_host_install_device(1, &other));
    CHECK(other == nullptr);
    CHECK(open_calls == previous_open_calls + 3);
    CHECK(open_devices == 1);
    CHECK(claims == 1);
    CHECK(get_max_count == 1);
    uint8_t sector[512] = {};
    REQUIRE(ESP_OK == scsi_cmd_read10(device, sector, 0, 1, sizeof(sector)));
    CHECK(sector[0] == 0xa5);

    REQUIRE(ESP_OK == msc_host_uninstall_device(device));
    device = nullptr;
    fixed_lun = -1;
    REQUIRE(ESP_OK == msc_host_probe_luns(1, 0, &info));
    CHECK(info.ready_lun_mask == 2);
    check_closed();
}

TEST_CASE_METHOD(LunFixture, "MSC probe distinguishes empty slots from failed LUNs", "[msc][lun]")
{
    msc_host_lun_info_t info = {};
    SECTION("All slots empty is a successful one-pass probe") {
        SECTION("MEDIUM NOT PRESENT") {
            empty_sense_ascq = 0x00;
        }
        SECTION("MEDIUM NOT PRESENT - TRAY CLOSED") {
            empty_sense_ascq = 0x01;
        }
        SECTION("MEDIUM NOT PRESENT - TRAY OPEN") {
            empty_sense_ascq = 0x02;
        }
        ready_luns = 0;
        const TickType_t started = xTaskGetTickCount();
        REQUIRE(ESP_OK == msc_host_probe_luns(1, 5000, &info));
        CHECK(xTaskGetTickCount() - started < pdMS_TO_TICKS(2500));
        CHECK(info.ready_lun_mask == 0);
        CHECK(info.failed_lun_mask == 0);
        CHECK(ready_count[0] == 1);
        CHECK(ready_count[1] == 1);
        CHECK(sense_count == 2);
    }
    SECTION("SCSI failures on one LUN do not prevent discovering others") {
        max_lun = 3;
        ready_luns = 15;
        inquiry_fail_luns = 1;
        tur_fail_luns = 2;
        capacity_fail_luns = 4;
        REQUIRE(ESP_OK == msc_host_probe_luns(1, 0, &info));
        CHECK(info.ready_lun_mask == 8);
        CHECK(info.failed_lun_mask == 7);
    }
    check_closed();
}

TEST_CASE_METHOD(LunFixture, "MSC temporary probe protects driver lifetime without exposing a device handle", "[msc][lun]")
{
    inject_probe_events = true;
    get_max_status = USB_TRANSFER_STATUS_NO_DEVICE;
    msc_host_lun_info_t info = { .ready_lun_mask = 0xffff, .failed_lun_mask = 0xffff, .max_lun = 15 };
    CHECK(ESP_ERR_MSC_INTERNAL == msc_host_probe_luns(1, 0, &info));
    CHECK(probe_lifetime_checks == 1);
    CHECK(public_events == 0);
    CHECK(info.max_lun == 0);
    CHECK(info.ready_lun_mask == 0);
    CHECK(info.failed_lun_mask == 0);
    check_closed();

    // The rejected uninstall must leave the driver available for later use.
    inject_probe_events = false;
    get_max_status = USB_TRANSFER_STATUS_COMPLETED;
    REQUIRE(ESP_OK == msc_host_install_device_lun(1, 1, &device));
}

TEST_CASE_METHOD(LunFixture, "MSC claim failure releases only the resources it owns", "[msc][lun]")
{
    claim_result = ESP_ERR_INVALID_STATE;
    SECTION("Temporary probe") {
        msc_host_lun_info_t info = { .ready_lun_mask = 0xffff, .failed_lun_mask = 0xffff, .max_lun = 15 };
        CHECK(ESP_ERR_INVALID_STATE == msc_host_probe_luns(1, 0, &info));
        CHECK(info.max_lun == 0);
        CHECK(info.ready_lun_mask == 0);
        CHECK(info.failed_lun_mask == 0);
    }
    SECTION("Explicit installation") {
        CHECK(ESP_ERR_INVALID_STATE == msc_host_install_device_lun(1, 1, &device));
        CHECK(device == nullptr);
    }
    CHECK(open_calls == 1);
    CHECK(release_calls == 0);
    CHECK(get_max_count == 0);
    check_closed();

    claim_result = ESP_OK;
    REQUIRE(ESP_OK == msc_host_install_device_lun(1, 1, &device));
}

TEST_CASE_METHOD(LunFixture, "MSC waits for transfer bookkeeping before releasing its interface", "[msc][lun]")
{
    SECTION("Temporary probe") {
        release_pending_once = true;
        msc_host_lun_info_t info = {};
        REQUIRE(ESP_OK == msc_host_probe_luns(1, 0, &info));
        CHECK(info.ready_lun_mask == 2);
    }
    SECTION("Installed device") {
        REQUIRE(ESP_OK == msc_host_install_device_lun(1, 1, &device));
        release_pending_once = true;
        REQUIRE(ESP_OK == msc_host_uninstall_device(device));
        device = nullptr;
    }
    CHECK(release_calls == 2);
    check_closed();
    REQUIRE(ESP_OK == msc_host_install_device_lun(1, 1, &device));
}

TEST_CASE_METHOD(LunFixture, "MSC probe requires supported sector geometry", "[msc][lun]")
{
    max_lun = 5;
    ready_luns = 0x3f;
    sector_sizes[0] = 512;
    sector_sizes[1] = 4096;
    sector_sizes[2] = 256;
    sector_sizes[3] = 768;
    sector_sizes[4] = 8192;
    sector_sizes[5] = 0;
    msc_host_lun_info_t info = {};
    REQUIRE(ESP_OK == msc_host_probe_luns(1, 0, &info));
    CHECK(info.ready_lun_mask == 3);
    CHECK(info.failed_lun_mask == 0x3c);
    check_closed();
}

TEST_CASE_METHOD(LunFixture, "MSC probe discards partial results after transport failure", "[msc][lun]")
{
    ready_luns = 3;
    transport_error_lun = 1;
    SECTION("TEST UNIT READY transport failure") {
        transport_error_opcode = TEST_UNIT_READY;
    }
    SECTION("INQUIRY transport failure") {
        transport_error_opcode = INQUIRY;
    }
    SECTION("READ CAPACITY transport failure") {
        transport_error_opcode = READ_CAPACITY;
    }
    msc_host_lun_info_t info = { .ready_lun_mask = 0xffff, .failed_lun_mask = 0xffff, .max_lun = 15 };
    CHECK(ESP_ERR_MSC_INTERNAL == msc_host_probe_luns(1, 0, &info));
    CHECK(capacity_count[0] == 1); // LUN zero was already discovered.
    CHECK(info.max_lun == 0);
    CHECK(info.ready_lun_mask == 0);
    CHECK(info.failed_lun_mask == 0);
    CHECK(sense_count == 0); // Transport errors must not be replaced by REQUEST SENSE results.
    check_closed();

    transport_error_lun = -1;
    REQUIRE(ESP_OK == msc_host_install_device_lun(1, 1, &device));
}

TEST_CASE_METHOD(LunFixture, "MSC probe rejects malformed CSWs instead of publishing partial results", "[msc][lun]")
{
    ready_luns = 3;
    esp_err_t expected = ESP_ERR_INVALID_RESPONSE;
    SECTION("Invalid signature") {
        csw_fault = CswFault::SIGNATURE;
    }
    SECTION("Mismatched command tag") {
        csw_fault = CswFault::TAG;
    }
    SECTION("Reserved status") {
        csw_fault = CswFault::RESERVED_STATUS;
    }
    SECTION("Phase error") {
        csw_fault = CswFault::PHASE_ERROR;
        expected = ESP_ERR_MSC_INTERNAL;
    }
    SECTION("Short CSW") {
        csw_fault = CswFault::SHORT_RESPONSE;
    }
    SECTION("Successful status with nonzero residue") {
        csw_fault = CswFault::RESIDUE;
    }
    msc_host_lun_info_t info = { .ready_lun_mask = 0xffff, .failed_lun_mask = 0xffff, .max_lun = 15 };
    CHECK(expected == msc_host_probe_luns(1, 0, &info));
    CHECK(capacity_count[0] == 1); // A previously ready LUN must not escape as a partial result.
    CHECK(info.max_lun == 0);
    CHECK(info.ready_lun_mask == 0);
    CHECK(info.failed_lun_mask == 0);
    CHECK(sense_count == 0);
    check_closed();

    // A malformed CSW or phase error requires BOT reset before another CBW.
    csw_fault = CswFault::NONE;
    fixed_lun = 1;
    REQUIRE(ESP_OK == msc_host_install_device_lun(1, 1, &device));
}

TEST_CASE_METHOD(LunFixture, "MSC probe shares its retry window and does not recheck resolved LUNs", "[msc][lun]")
{
    SECTION("Media are becoming ready") {
        pending_sense_key = 0x02;
    }
    SECTION("Unit attention needs REQUEST SENSE before retrying") {
        pending_sense_key = 0x06;
    }
    max_lun = 15;
    ready_luns = 1;
    tur_fail_luns = 4;
    delayed_lun = 1;
    pending_luns = 1 << 15;
    const TickType_t started = xTaskGetTickCount();
    msc_host_lun_info_t info = {};
    REQUIRE(ESP_OK == msc_host_probe_luns(1, 150, &info));
    const TickType_t elapsed = xTaskGetTickCount() - started;
    CHECK(elapsed >= pdMS_TO_TICKS(150));
    CHECK(elapsed < pdMS_TO_TICKS(1000)); // Not a separate 150 ms wait for every slot.
    CHECK(info.ready_lun_mask == 3);
    CHECK(info.failed_lun_mask == 4);
    CHECK(ready_count[0] == 1);
    CHECK(ready_count[1] == 2);
    CHECK(ready_count[2] == 1);
    for (unsigned i = 3; i < 15; ++i) {
        CHECK(ready_count[i] == 1); // Empty slots never consume the retry window.
    }
    CHECK(ready_count[15] >= 2);
    for (unsigned i = 0; i < 16; ++i) {
        CHECK(inquiry_count[i] == 1);
    }
    check_closed();
}

TEST_CASE_METHOD(LunFixture, "MSC zero-budget probe reports only immediately ready LUNs", "[msc][lun]")
{
    SECTION("Media are becoming ready") {
        pending_sense_key = 0x02;
    }
    SECTION("Unit attention needs another TEST UNIT READY") {
        pending_sense_key = 0x06;
    }
    ready_luns = 1;
    delayed_lun = 1;
    msc_host_lun_info_t info = {};
    REQUIRE(ESP_OK == msc_host_probe_luns(1, 0, &info));
    CHECK(info.ready_lun_mask == 1);
    CHECK(info.failed_lun_mask == 0);
    CHECK(ready_count[0] == 1);
    CHECK(ready_count[1] == 1);
    CHECK(sense_count == 1);
    check_closed();
}

TEST_CASE_METHOD(LunFixture, "MSC probe returns when the last initializing LUN becomes ready", "[msc][lun]")
{
    ready_luns = 1;
    delayed_lun = 1;
    msc_host_lun_info_t info = {};
    const TickType_t started = xTaskGetTickCount();
    REQUIRE(ESP_OK == msc_host_probe_luns(1, 5000, &info));
    CHECK(xTaskGetTickCount() - started < pdMS_TO_TICKS(2500));
    CHECK(info.ready_lun_mask == 3);
    CHECK(info.failed_lun_mask == 0);
    CHECK(ready_count[0] == 1);
    CHECK(ready_count[1] == 2);
    CHECK(sense_count == 1);
    check_closed();
}

TEST_CASE_METHOD(LunFixture, "MSC synchronizes transport before the first command of each session", "[msc][lun]")
{
    max_lun = 0;
    ready_luns = 1;
    msc_host_lun_info_t info = {};
    // INQUIRY/TUR/CAPACITY leave three OUT and five IN packets: both DATA1.
    REQUIRE(ESP_OK == msc_host_probe_luns(1, 0, &info));
    CHECK(info.ready_lun_mask == 1);
    CHECK(info.failed_lun_mask == 0);
    REQUIRE(clear_count == 2);
    CHECK(clears[0].endpoint == 0x81);
    CHECK(clears[1].endpoint == 0x02);
    CHECK(clears[0].bulk_count == 0);
    CHECK(clears[1].bulk_count == 0);
    CHECK(reset_count == 1);
    const unsigned probe_bulk_count = bulk_count;
    check_closed();

    // Reclaim resets the host PID; reset and CLEAR_FEATURE synchronize the device.
    REQUIRE(ESP_OK == msc_host_install_device_lun(1, 0, &device));
    REQUIRE(clear_count == 4);
    CHECK(clears[2].endpoint == 0x81);
    CHECK(clears[3].endpoint == 0x02);
    CHECK(clears[2].bulk_count == probe_bulk_count);
    CHECK(clears[3].bulk_count == probe_bulk_count);
    CHECK(reset_count == 2);
    CHECK(ready_count[0] == 2);
}

TEST_CASE_METHOD(LunFixture, "MSC probe releases its session when transport initialization fails", "[msc][lun]")
{
    max_lun = 0;
    ready_luns = 1;
    unsigned expected_clears = 0;
    SECTION("Mass Storage Reset fails") {
        reset_status = USB_TRANSFER_STATUS_ERROR;
    }
    SECTION("Bulk IN clear fails") {
        clear_fail_endpoint = 0x81;
        expected_clears = 1;
    }
    SECTION("Bulk OUT clear fails") {
        clear_fail_endpoint = 0x02;
        expected_clears = 2;
    }
    msc_host_lun_info_t info = { .ready_lun_mask = 0xffff, .failed_lun_mask = 0xffff, .max_lun = 15 };
    CHECK(ESP_ERR_MSC_INTERNAL == msc_host_probe_luns(1, 0, &info));
    CHECK(info.max_lun == 0);
    CHECK(info.ready_lun_mask == 0);
    CHECK(info.failed_lun_mask == 0);
    REQUIRE(clear_count == expected_clears);
    if (expected_clears != 0) {
        CHECK(clears[0].endpoint == 0x81);
        CHECK(clears[clear_count - 1].endpoint == clear_fail_endpoint);
    }
    CHECK(bulk_count == 0);
    CHECK(get_max_count == 0);
    check_closed();

    reset_status = USB_TRANSFER_STATUS_COMPLETED;
    clear_fail_endpoint = 0;
    REQUIRE(ESP_OK == msc_host_probe_luns(1, 0, &info));
    CHECK(info.ready_lun_mask == 1);
    check_closed();
}
