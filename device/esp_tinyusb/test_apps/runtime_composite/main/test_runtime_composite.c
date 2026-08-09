/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED

#include <string.h>
#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"
#include "class/hid/hid_device.h"
#include "runtime_composite.h"
#include "descriptors_control.h"

#define TEST_RUNTIME_BUILD_ITERATIONS       64
#define TEST_RUNTIME_BUILD_TIME_LIMIT_US    200000
#define TEST_RUNTIME_HEAP_DRIFT_LIMIT       256

static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};

static int s_hid_ctx;
static int s_vendor_ctx0;
static int s_vendor_ctx1;
static int s_cdc_ctx;
static int s_ncm_ctx;

static const char *s_runtime_strings[] = {
    (char[]){0x09, 0x04},
    "Espressif",
    "Runtime Composite",
    "123456",
    "Function 4",
    "Function 5",
    "Function 6",
    "Function 7",
    "Function 8",
    "Function 9",
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    TEST_ASSERT_EQUAL_PTR(&s_hid_ctx, tinyusb_runtime_get_callback_ctx(TINYUSB_RUNTIME_CLASS_HID, instance));
    return s_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

#if (TUSB_VERSION_MINOR >= 17)
void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
#else
void tud_vendor_rx_cb(uint8_t itf)
#endif
{
#if (TUSB_VERSION_MINOR >= 17)
    (void)buffer;
    (void)bufsize;
#endif
    if (itf == 0) {
        TEST_ASSERT_EQUAL_PTR(&s_vendor_ctx0, tinyusb_runtime_get_callback_ctx(TINYUSB_RUNTIME_CLASS_VENDOR, 0));
    } else if (itf == 1) {
        TEST_ASSERT_EQUAL_PTR(&s_vendor_ctx1, tinyusb_runtime_get_callback_ctx(TINYUSB_RUNTIME_CLASS_VENDOR, 1));
    }
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    (void)rhport;
    (void)stage;
    (void)request;
    return false;
}

static esp_err_t net_recv_cb(void *buffer, uint16_t len, void *ctx)
{
    (void)buffer;
    (void)len;
    TEST_ASSERT_EQUAL_PTR(&s_ncm_ctx, ctx);
    return ESP_OK;
}

static void net_free_tx_cb(void *buffer, void *ctx)
{
    (void)buffer;
    TEST_ASSERT_EQUAL_PTR(&s_ncm_ctx, ctx);
}

static void install_net_driver(void)
{
    tinyusb_net_config_t net_config = {
        .mac_addr = { 0x02, 0x00, 0x00, 0x12, 0x34, 0x56 },
        .on_recv_callback = net_recv_cb,
        .free_tx_buffer = net_free_tx_cb,
        .user_context = &s_ncm_ctx,
    };
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_net_init(&net_config));
}

static tinyusb_runtime_config_t runtime_cfg(const tinyusb_runtime_function_t *functions, size_t function_count)
{
    return (tinyusb_runtime_config_t) {
        .functions = functions,
        .function_count = function_count,
        .string = s_runtime_strings,
        .string_count = sizeof(s_runtime_strings) / sizeof(s_runtime_strings[0]),
        .power_ma = 100,
    };
}

static uint8_t config_interface_count(const tinyusb_runtime_desc_t *desc)
{
    const tusb_desc_configuration_t *cfg = (const tusb_desc_configuration_t *)desc->desc_cfg.full_speed_config;
    return cfg->bNumInterfaces;
}

static uint16_t config_total_len(const tinyusb_runtime_desc_t *desc)
{
    const tusb_desc_configuration_t *cfg = (const tusb_desc_configuration_t *)desc->desc_cfg.full_speed_config;
    return cfg->wTotalLength;
}

static void expect_runtime_build_ok(const tinyusb_runtime_config_t *cfg, tinyusb_runtime_desc_t *desc)
{
    memset(desc, 0, sizeof(*desc));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_runtime_descriptor_build(TINYUSB_PORT_FULL_SPEED_0, cfg, desc));
    TEST_ASSERT_NOT_NULL(desc->desc_cfg.device);
    TEST_ASSERT_NOT_NULL(desc->desc_cfg.full_speed_config);
    TEST_ASSERT_NOT_NULL(desc->desc_cfg.string);
}

TEST_CASE("Runtime composite: descriptor builder validates NULL and empty inputs", "[runtime_composite]")
{
    tinyusb_runtime_desc_t desc;
    const tinyusb_runtime_config_t empty_cfg = runtime_cfg(NULL, 0);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tinyusb_runtime_descriptor_build(TINYUSB_PORT_FULL_SPEED_0, NULL, &desc));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tinyusb_runtime_descriptor_build(TINYUSB_PORT_FULL_SPEED_0, &empty_cfg, NULL));

    tinyusb_runtime_config_t bad_cfg = runtime_cfg(NULL, 1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tinyusb_runtime_descriptor_build(TINYUSB_PORT_FULL_SPEED_0, &bad_cfg, &desc));

    expect_runtime_build_ok(&empty_cfg, &desc);
    TEST_ASSERT_EQUAL(TUD_CONFIG_DESC_LEN, config_total_len(&desc));
    TEST_ASSERT_EQUAL(0, config_interface_count(&desc));
    tinyusb_runtime_descriptor_free(&desc);
}

TEST_CASE("Runtime composite: descriptor builder composes mixed class layout", "[runtime_composite]")
{
    const tinyusb_runtime_function_t functions[] = {
        {
            .type = TINYUSB_RUNTIME_CLASS_CDC_ACM,
            .instance = 0,
            .callback_ctx = &s_cdc_ctx,
        },
        {
            .type = TINYUSB_RUNTIME_CLASS_MSC,
            .instance = 0,
        },
        {
            .type = TINYUSB_RUNTIME_CLASS_HID,
            .instance = 0,
            .descriptor.hid = {
                .protocol = HID_ITF_PROTOCOL_KEYBOARD,
                .report_descriptor_len = sizeof(s_hid_report_descriptor),
                .ep_interval = 10,
            },
            .callback_ctx = &s_hid_ctx,
        },
        {
            .type = TINYUSB_RUNTIME_CLASS_MIDI,
            .instance = 0,
        },
    };
    tinyusb_runtime_config_t cfg = runtime_cfg(functions, sizeof(functions) / sizeof(functions[0]));
    cfg.id_product = 0x5a5a;
    cfg.bcd_device = 0x0201;

    tinyusb_runtime_desc_t desc;
    expect_runtime_build_ok(&cfg, &desc);
    TEST_ASSERT_EQUAL(6, config_interface_count(&desc));
    TEST_ASSERT_GREATER_THAN(TUD_CONFIG_DESC_LEN, config_total_len(&desc));
    TEST_ASSERT_EQUAL(TUSB_CLASS_MISC, desc.desc_cfg.device->bDeviceClass);
    TEST_ASSERT_EQUAL(0x5a5a, desc.desc_cfg.device->idProduct);
    TEST_ASSERT_EQUAL(0x0201, desc.desc_cfg.device->bcdDevice);
    TEST_ASSERT_EQUAL_PTR(&s_hid_ctx, tinyusb_runtime_get_callback_ctx(TINYUSB_RUNTIME_CLASS_HID, 0));
    tinyusb_runtime_descriptor_free(&desc);
}

TEST_CASE("Runtime composite: descriptor builder rejects boundary violations", "[runtime_composite]")
{
    tinyusb_runtime_desc_t desc;

    const tinyusb_runtime_function_t over_instance[] = {
        {
            .type = TINYUSB_RUNTIME_CLASS_VENDOR,
            .instance = CFG_TUD_VENDOR,
        },
    };
    tinyusb_runtime_config_t cfg = runtime_cfg(over_instance, sizeof(over_instance) / sizeof(over_instance[0]));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, tinyusb_runtime_descriptor_build(TINYUSB_PORT_FULL_SPEED_0, &cfg, &desc));

    const tinyusb_runtime_function_t raw_empty[] = {
        {
            .type = TINYUSB_RUNTIME_CLASS_RAW,
            .descriptor.raw = {
                .interface_count = 1,
            },
        },
    };
    cfg = runtime_cfg(raw_empty, sizeof(raw_empty) / sizeof(raw_empty[0]));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tinyusb_runtime_descriptor_build(TINYUSB_PORT_FULL_SPEED_0, &cfg, &desc));

    const tinyusb_runtime_function_t endpoint_heavy[] = {
        { .type = TINYUSB_RUNTIME_CLASS_CDC_ACM, .instance = 0 },
        { .type = TINYUSB_RUNTIME_CLASS_CDC_ACM, .instance = 1 },
        { .type = TINYUSB_RUNTIME_CLASS_VENDOR, .instance = 0 },
        { .type = TINYUSB_RUNTIME_CLASS_VENDOR, .instance = 1 },
        { .type = TINYUSB_RUNTIME_CLASS_NET_NCM, .instance = 0 },
    };
    cfg = runtime_cfg(endpoint_heavy, sizeof(endpoint_heavy) / sizeof(endpoint_heavy[0]));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, tinyusb_runtime_descriptor_build(TINYUSB_PORT_FULL_SPEED_0, &cfg, &desc));

    const char *too_many_strings[USB_STRING_DESCRIPTOR_ARRAY_SIZE + 1] = { 0 };
    cfg = runtime_cfg(NULL, 0);
    cfg.string = too_many_strings;
    cfg.string_count = USB_STRING_DESCRIPTOR_ARRAY_SIZE + 1;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, tinyusb_runtime_descriptor_build(TINYUSB_PORT_FULL_SPEED_0, &cfg, &desc));
}

TEST_CASE("Runtime composite: install and reconfigure class combinations", "[runtime_composite]")
{
    install_net_driver();

    const tinyusb_runtime_function_t initial_functions[] = {
        {
            .type = TINYUSB_RUNTIME_CLASS_CDC_ACM,
            .instance = 0,
            .callback_ctx = &s_cdc_ctx,
        },
        {
            .type = TINYUSB_RUNTIME_CLASS_HID,
            .instance = 0,
            .descriptor.hid = {
                .protocol = HID_ITF_PROTOCOL_KEYBOARD,
                .report_descriptor_len = sizeof(s_hid_report_descriptor),
                .ep_interval = 10,
            },
            .callback_ctx = &s_hid_ctx,
        },
        {
            .type = TINYUSB_RUNTIME_CLASS_VENDOR,
            .instance = 0,
            .callback_ctx = &s_vendor_ctx0,
        },
    };

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tinyusb_runtime_config_t cfg = runtime_cfg(initial_functions, sizeof(initial_functions) / sizeof(initial_functions[0]));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install_runtime(&tusb_cfg, &cfg));
    TEST_ASSERT_EQUAL_PTR(&s_hid_ctx, tinyusb_runtime_get_callback_ctx(TINYUSB_RUNTIME_CLASS_HID, 0));
    TEST_ASSERT_EQUAL_PTR(&s_vendor_ctx0, tinyusb_runtime_get_callback_ctx(TINYUSB_RUNTIME_CLASS_VENDOR, 0));
    TEST_ASSERT_NULL(tinyusb_runtime_get_callback_ctx(TINYUSB_RUNTIME_CLASS_VENDOR, 1));

    const tinyusb_runtime_function_t reconfigured_functions[] = {
        {
            .type = TINYUSB_RUNTIME_CLASS_CDC_ACM,
            .instance = 1,
            .callback_ctx = &s_cdc_ctx,
        },
        {
            .type = TINYUSB_RUNTIME_CLASS_VENDOR,
            .instance = 1,
            .callback_ctx = &s_vendor_ctx1,
        },
        {
            .type = TINYUSB_RUNTIME_CLASS_NET_NCM,
            .instance = 0,
            .callback_ctx = &s_ncm_ctx,
        },
    };

    cfg = runtime_cfg(reconfigured_functions, sizeof(reconfigured_functions) / sizeof(reconfigured_functions[0]));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_reconfigure(&cfg));
    TEST_ASSERT_NULL(tinyusb_runtime_get_callback_ctx(TINYUSB_RUNTIME_CLASS_HID, 0));
    TEST_ASSERT_NULL(tinyusb_runtime_get_callback_ctx(TINYUSB_RUNTIME_CLASS_VENDOR, 0));
    TEST_ASSERT_EQUAL_PTR(&s_vendor_ctx1, tinyusb_runtime_get_callback_ctx(TINYUSB_RUNTIME_CLASS_VENDOR, 1));
    TEST_ASSERT_EQUAL_PTR(&s_ncm_ctx, tinyusb_runtime_get_callback_ctx(TINYUSB_RUNTIME_CLASS_NET_NCM, 0));

    tinyusb_net_deinit();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}

TEST_CASE("Runtime composite: failed reconfigure preserves active callback map", "[runtime_composite]")
{
    const tinyusb_runtime_function_t functions[] = {
        {
            .type = TINYUSB_RUNTIME_CLASS_HID,
            .instance = 0,
            .descriptor.hid = {
                .protocol = HID_ITF_PROTOCOL_KEYBOARD,
                .report_descriptor_len = sizeof(s_hid_report_descriptor),
                .ep_interval = 10,
            },
            .callback_ctx = &s_hid_ctx,
        },
    };

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tinyusb_runtime_config_t cfg = runtime_cfg(functions, sizeof(functions) / sizeof(functions[0]));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install_runtime(&tusb_cfg, &cfg));
    TEST_ASSERT_EQUAL_PTR(&s_hid_ctx, tinyusb_runtime_get_callback_ctx(TINYUSB_RUNTIME_CLASS_HID, 0));

    const tinyusb_runtime_function_t invalid_functions[] = {
        {
            .type = TINYUSB_RUNTIME_CLASS_VENDOR,
            .instance = CFG_TUD_VENDOR,
        },
    };
    cfg = runtime_cfg(invalid_functions, sizeof(invalid_functions) / sizeof(invalid_functions[0]));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, tinyusb_driver_reconfigure(&cfg));
    TEST_ASSERT_EQUAL_PTR(&s_hid_ctx, tinyusb_runtime_get_callback_ctx(TINYUSB_RUNTIME_CLASS_HID, 0));

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}

TEST_CASE("Runtime composite: rejects class instance over Kconfig limit", "[runtime_composite]")
{
    const tinyusb_runtime_function_t functions[] = {
        {
            .type = TINYUSB_RUNTIME_CLASS_CDC_ACM,
            .instance = CFG_TUD_CDC,
        },
    };
    tinyusb_runtime_config_t cfg = runtime_cfg(functions, sizeof(functions) / sizeof(functions[0]));
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, tinyusb_driver_install_runtime(&tusb_cfg, &cfg));
}

TEST_CASE("Runtime composite: raw descriptor fragment can be composed", "[runtime_composite]")
{
    static const uint8_t raw_fs[] = {
        TUD_VENDOR_DESCRIPTOR(0, 4, 0x01, 0x81, 64),
    };

    const tinyusb_runtime_function_t functions[] = {
        {
            .type = TINYUSB_RUNTIME_CLASS_RAW,
            .descriptor.raw = {
                .full_speed = raw_fs,
                .full_speed_len = sizeof(raw_fs),
                .interface_count = 1,
                .in_endpoint_count = 1,
                .out_endpoint_count = 1,
            },
            .callback_ctx = &s_vendor_ctx0,
        },
    };

    tinyusb_runtime_config_t cfg = runtime_cfg(functions, sizeof(functions) / sizeof(functions[0]));
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install_runtime(&tusb_cfg, &cfg));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}

TEST_CASE("Runtime composite: repeated descriptor build is fast and frees heap", "[runtime_composite]")
{
    const tinyusb_runtime_function_t functions[] = {
        {
            .type = TINYUSB_RUNTIME_CLASS_CDC_ACM,
            .instance = 0,
            .callback_ctx = &s_cdc_ctx,
        },
        {
            .type = TINYUSB_RUNTIME_CLASS_HID,
            .instance = 0,
            .descriptor.hid = {
                .protocol = HID_ITF_PROTOCOL_KEYBOARD,
                .report_descriptor_len = sizeof(s_hid_report_descriptor),
                .ep_interval = 10,
            },
            .callback_ctx = &s_hid_ctx,
        },
        {
            .type = TINYUSB_RUNTIME_CLASS_VENDOR,
            .instance = 0,
            .callback_ctx = &s_vendor_ctx0,
        },
    };
    const tinyusb_runtime_config_t cfg = runtime_cfg(functions, sizeof(functions) / sizeof(functions[0]));
    const size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    const int64_t start_us = esp_timer_get_time();

    for (int i = 0; i < TEST_RUNTIME_BUILD_ITERATIONS; i++) {
        tinyusb_runtime_desc_t desc;
        expect_runtime_build_ok(&cfg, &desc);
        TEST_ASSERT_EQUAL(4, config_interface_count(&desc));
        tinyusb_runtime_descriptor_free(&desc);
    }

    const int64_t elapsed_us = esp_timer_get_time() - start_us;
    const size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    TEST_ASSERT_LESS_THAN(TEST_RUNTIME_BUILD_TIME_LIMIT_US, elapsed_us);
    TEST_ASSERT_INT_WITHIN(TEST_RUNTIME_HEAP_DRIFT_LIMIT, heap_before, heap_after);
}

#endif // SOC_USB_OTG_SUPPORTED
