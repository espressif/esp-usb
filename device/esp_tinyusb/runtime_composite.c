/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "runtime_composite.h"
#include "descriptors_control.h"
#include "usb_descriptors.h"
#include "sdkconfig.h"

static const char *TAG = "tusb_runtime";

#define RUNTIME_PID_BASE                0x5000
#define RUNTIME_DEFAULT_POWER_MA        100
#define RUNTIME_DEFAULT_CONFIG_ATTR     TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP
#define RUNTIME_MAX_EP_NUM              6
#define RUNTIME_MAX_OUT_EP_NUM          5
#define RUNTIME_DFU_ALT_COUNT_DEFAULT   1
#define RUNTIME_DFU_FUNC_ATTRS          (DFU_ATTR_CAN_DOWNLOAD | DFU_ATTR_CAN_UPLOAD | DFU_ATTR_MANIFESTATION_TOLERANT)

#ifndef CFG_TUD_NET_MTU
#define CFG_TUD_NET_MTU                 1514
#endif

typedef struct {
    uint8_t *buf;
    uint16_t len;
    uint16_t cap;
} desc_builder_t;

typedef struct {
    uint8_t next_itf;
    uint8_t next_ep;
    uint8_t next_str;
    uint16_t class_bitmap;
    bool uses_iad;
} compose_state_t;

typedef struct {
    tinyusb_runtime_class_t type;
    uint8_t instance;
    void *ctx;
} runtime_callback_slot_t;

typedef struct {
    runtime_callback_slot_t slots[CONFIG_TINYUSB_CDC_COUNT +
                                                           CONFIG_TINYUSB_MSC_ENABLED +
                                                           CONFIG_TINYUSB_HID_COUNT +
                                                           CONFIG_TINYUSB_MIDI_COUNT +
                                                           CONFIG_TINYUSB_VENDOR_COUNT +
                                                           CONFIG_TINYUSB_NET_MODE_ECM_RNDIS +
                                                           CONFIG_TINYUSB_NET_MODE_NCM +
                                                           CONFIG_TINYUSB_DFU_MODE_DFU +
                                                           CONFIG_TINYUSB_DFU_MODE_DFU_RUNTIME +
                                                           CONFIG_TINYUSB_BTH_ENABLED + 1];
    size_t slot_count;
} runtime_callback_map_t;

static runtime_callback_map_t s_cb_map;
static runtime_callback_map_t s_build_cb_map;
static bool s_runtime_active;
static uint8_t s_net_mac_string_id;
static uint8_t s_build_net_mac_string_id;

static esp_err_t builder_init(desc_builder_t *builder, uint16_t cap)
{
    builder->buf = calloc(1, cap);
    ESP_RETURN_ON_FALSE(builder->buf, ESP_ERR_NO_MEM, TAG, "No memory for descriptor");
    builder->cap = cap;
    builder->len = 0;
    return ESP_OK;
}

static esp_err_t builder_append(desc_builder_t *builder, const void *data, uint16_t len)
{
    ESP_RETURN_ON_FALSE(builder->len + len <= builder->cap, ESP_ERR_NO_MEM, TAG, "Descriptor buffer too small");
    memcpy(builder->buf + builder->len, data, len);
    builder->len += len;
    return ESP_OK;
}

static esp_err_t builder_reserve_config(desc_builder_t *builder)
{
    uint8_t empty[TUD_CONFIG_DESC_LEN] = { 0 };
    return builder_append(builder, empty, sizeof(empty));
}

static void builder_finish_config(desc_builder_t *builder, uint8_t itf_count, uint8_t attrs, uint16_t power_ma)
{
    uint8_t header[] = {
        TUD_CONFIG_DESCRIPTOR(1, itf_count, 0, builder->len, attrs, power_ma)
    };
    memcpy(builder->buf, header, sizeof(header));
}

#if (SOC_USB_OTG_PERIPH_NUM > 1)
static bool port_is_hs(tinyusb_port_t port)
{
    return port == TINYUSB_PORT_HIGH_SPEED_0;
}
#elif CONFIG_IDF_TARGET_ESP32S31
static bool port_is_hs(tinyusb_port_t port)
{
    (void)port;
    return true;
}
#else
#define port_is_hs(port)    false
#endif

static esp_err_t alloc_pair_ep(compose_state_t *state, uint8_t *ep)
{
    ESP_RETURN_ON_FALSE(state->next_ep <= RUNTIME_MAX_OUT_EP_NUM, ESP_ERR_NOT_SUPPORTED, TAG, "No OUT endpoint left");
    *ep = state->next_ep++;
    return ESP_OK;
}

static esp_err_t alloc_in_ep(compose_state_t *state, uint8_t *ep)
{
    ESP_RETURN_ON_FALSE(state->next_ep <= RUNTIME_MAX_EP_NUM, ESP_ERR_NOT_SUPPORTED, TAG, "No IN endpoint left");
    *ep = state->next_ep++;
    return ESP_OK;
}

static uint8_t pick_string_index(compose_state_t *state, const tinyusb_runtime_function_t *fn)
{
    if (fn->string_index != 0) {
        return fn->string_index;
    }
    return state->next_str++;
}

static void add_callback_slot(const tinyusb_runtime_function_t *fn)
{
    if (s_build_cb_map.slot_count >= sizeof(s_build_cb_map.slots) / sizeof(s_build_cb_map.slots[0])) {
        return;
    }
    s_build_cb_map.slots[s_build_cb_map.slot_count++] = (runtime_callback_slot_t) {
        .type = fn->type,
        .instance = fn->instance,
        .ctx = fn->callback_ctx,
    };
}

static uint16_t class_bit(tinyusb_runtime_class_t type, uint8_t instance)
{
    return 1U << (((uint8_t)type + instance) & 0x0f);
}

static esp_err_t append_raw(desc_builder_t *builder, const tinyusb_runtime_raw_descriptor_t *raw, bool high_speed)
{
    const uint8_t *desc = high_speed && raw->high_speed ? raw->high_speed : raw->full_speed;
    uint16_t len = high_speed && raw->high_speed ? raw->high_speed_len : raw->full_speed_len;
    ESP_RETURN_ON_FALSE(desc && len, ESP_ERR_INVALID_ARG, TAG, "Raw descriptor fragment is empty");
    return builder_append(builder, desc, len);
}

static esp_err_t append_function(desc_builder_t *builder, compose_state_t *state,
                                 const tinyusb_runtime_function_t *fn, bool high_speed)
{
    const uint16_t bulk_ep_size = high_speed ? 512 : 64;
    const uint8_t intr_ep_size = high_speed ? 64 : 8;
    esp_err_t ret;
    uint8_t ep;
    uint8_t ep_notif;
    uint8_t str_idx = pick_string_index(state, fn);

    switch (fn->type) {
    case TINYUSB_RUNTIME_CLASS_CDC_ACM:
#if CFG_TUD_CDC > 0
        ESP_RETURN_ON_FALSE(fn->instance < CFG_TUD_CDC, ESP_ERR_NOT_SUPPORTED, TAG, "CDC instance exceeds CFG_TUD_CDC");
        ret = alloc_in_ep(state, &ep_notif);
        ESP_RETURN_ON_ERROR(ret, TAG, "No CDC notification endpoint");
        ret = alloc_pair_ep(state, &ep);
        ESP_RETURN_ON_ERROR(ret, TAG, "No CDC data endpoint");
        {
            uint8_t desc[] = {
                TUD_CDC_DESCRIPTOR(state->next_itf, str_idx, 0x80 | ep_notif, intr_ep_size, ep, 0x80 | ep, bulk_ep_size)
            };
            ESP_RETURN_ON_ERROR(builder_append(builder, desc, sizeof(desc)), TAG, "Append CDC descriptor");
        }
        state->next_itf += 2;
        state->uses_iad = true;
        break;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif

    case TINYUSB_RUNTIME_CLASS_MSC:
#if CFG_TUD_MSC
        ret = alloc_pair_ep(state, &ep);
        ESP_RETURN_ON_ERROR(ret, TAG, "No MSC endpoint");
        {
            uint8_t desc[] = {
                TUD_MSC_DESCRIPTOR(state->next_itf, str_idx, ep, 0x80 | ep, bulk_ep_size)
            };
            ESP_RETURN_ON_ERROR(builder_append(builder, desc, sizeof(desc)), TAG, "Append MSC descriptor");
        }
        state->next_itf++;
        break;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif

    case TINYUSB_RUNTIME_CLASS_HID:
#if CFG_TUD_HID > 0
        ESP_RETURN_ON_FALSE(fn->instance < CFG_TUD_HID, ESP_ERR_NOT_SUPPORTED, TAG, "HID instance exceeds CFG_TUD_HID");
        ret = alloc_in_ep(state, &ep);
        ESP_RETURN_ON_ERROR(ret, TAG, "No HID endpoint");
        {
            uint8_t desc[] = {
                TUD_HID_DESCRIPTOR(state->next_itf, str_idx, fn->descriptor.hid.protocol,
                                   fn->descriptor.hid.report_descriptor_len,
                                   0x80 | ep, 16, fn->descriptor.hid.ep_interval ? fn->descriptor.hid.ep_interval : 10)
            };
            ESP_RETURN_ON_ERROR(builder_append(builder, desc, sizeof(desc)), TAG, "Append HID descriptor");
        }
        state->next_itf++;
        break;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif

    case TINYUSB_RUNTIME_CLASS_MIDI:
#if CFG_TUD_MIDI > 0
        ESP_RETURN_ON_FALSE(fn->instance < CFG_TUD_MIDI, ESP_ERR_NOT_SUPPORTED, TAG, "MIDI instance exceeds CFG_TUD_MIDI");
        ret = alloc_pair_ep(state, &ep);
        ESP_RETURN_ON_ERROR(ret, TAG, "No MIDI endpoint");
        {
            uint8_t desc[] = {
                TUD_MIDI_DESCRIPTOR(state->next_itf, str_idx, ep, 0x80 | ep, bulk_ep_size)
            };
            ESP_RETURN_ON_ERROR(builder_append(builder, desc, sizeof(desc)), TAG, "Append MIDI descriptor");
        }
        state->next_itf += 2;
        break;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif

    case TINYUSB_RUNTIME_CLASS_VENDOR:
#if CFG_TUD_VENDOR > 0
        const uint16_t vendor_ep_size = high_speed ? 512 : 64;
        ESP_RETURN_ON_FALSE(fn->instance < CFG_TUD_VENDOR, ESP_ERR_NOT_SUPPORTED, TAG, "Vendor instance exceeds CFG_TUD_VENDOR");
        ret = alloc_pair_ep(state, &ep);
        ESP_RETURN_ON_ERROR(ret, TAG, "No Vendor endpoint");
        {
            uint8_t desc[] = {
                TUD_VENDOR_DESCRIPTOR(state->next_itf, str_idx, ep, 0x80 | ep, vendor_ep_size)
            };
            ESP_RETURN_ON_ERROR(builder_append(builder, desc, sizeof(desc)), TAG, "Append Vendor descriptor");
        }
        state->next_itf++;
        break;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif

    case TINYUSB_RUNTIME_CLASS_NET_NCM:
#if CFG_TUD_NCM
        ret = alloc_in_ep(state, &ep_notif);
        ESP_RETURN_ON_ERROR(ret, TAG, "No NCM notification endpoint");
        ret = alloc_pair_ep(state, &ep);
        ESP_RETURN_ON_ERROR(ret, TAG, "No NCM data endpoint");
        {
            uint8_t desc[] = {
                TUD_CDC_NCM_DESCRIPTOR(state->next_itf, str_idx, str_idx + 1, 0x80 | ep_notif,
                                       high_speed ? 64 : 64, ep, 0x80 | ep, bulk_ep_size, CFG_TUD_NET_MTU)
            };
            ESP_RETURN_ON_ERROR(builder_append(builder, desc, sizeof(desc)), TAG, "Append NCM descriptor");
        }
        state->next_itf += 2;
        s_build_net_mac_string_id = str_idx + 1;
        state->next_str++;
        state->uses_iad = true;
        break;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif

    case TINYUSB_RUNTIME_CLASS_NET_ECM_RNDIS:
#if CFG_TUD_ECM_RNDIS
        ret = alloc_in_ep(state, &ep_notif);
        ESP_RETURN_ON_ERROR(ret, TAG, "No ECM notification endpoint");
        ret = alloc_pair_ep(state, &ep);
        ESP_RETURN_ON_ERROR(ret, TAG, "No ECM data endpoint");
        {
            uint8_t desc[] = {
                TUD_CDC_ECM_DESCRIPTOR(state->next_itf, str_idx, str_idx + 1, 0x80 | ep_notif,
                                       high_speed ? 512 : 64, ep, 0x80 | ep, bulk_ep_size, CFG_TUD_NET_MTU)
            };
            ESP_RETURN_ON_ERROR(builder_append(builder, desc, sizeof(desc)), TAG, "Append ECM descriptor");
        }
        state->next_itf += 2;
        s_build_net_mac_string_id = str_idx + 1;
        state->next_str++;
        state->uses_iad = true;
        break;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif

    case TINYUSB_RUNTIME_CLASS_DFU:
#if CFG_TUD_DFU
    {
        uint8_t desc[] = {
            TUD_DFU_DESCRIPTOR(state->next_itf, RUNTIME_DFU_ALT_COUNT_DEFAULT, str_idx,
                               RUNTIME_DFU_FUNC_ATTRS, 1000, CFG_TUD_DFU_XFER_BUFSIZE)
        };
        ESP_RETURN_ON_ERROR(builder_append(builder, desc, sizeof(desc)), TAG, "Append DFU descriptor");
    }
    state->next_itf += RUNTIME_DFU_ALT_COUNT_DEFAULT;
    break;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif

    case TINYUSB_RUNTIME_CLASS_BTH:
#if CFG_TUD_BTH
        ret = alloc_in_ep(state, &ep_notif);
        ESP_RETURN_ON_ERROR(ret, TAG, "No BTH event endpoint");
        ret = alloc_pair_ep(state, &ep);
        ESP_RETURN_ON_ERROR(ret, TAG, "No BTH data endpoint");
        {
            uint8_t desc[] = {
                TUD_BTH_DESCRIPTOR(state->next_itf, str_idx, 0x80 | ep_notif, 16, 1,
                                   0x80 | ep, ep, bulk_ep_size, high_speed ? 1024 : 64, high_speed ? 1024 : 64)
            };
            ESP_RETURN_ON_ERROR(builder_append(builder, desc, sizeof(desc)), TAG, "Append BTH descriptor");
        }
        state->next_itf++;
        break;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif

    case TINYUSB_RUNTIME_CLASS_DFU_RUNTIME:
    case TINYUSB_RUNTIME_CLASS_RAW:
        ESP_RETURN_ON_ERROR(append_raw(builder, &fn->descriptor.raw, high_speed), TAG, "Append raw descriptor");
        state->next_itf += fn->descriptor.raw.interface_count;
        state->next_ep += fn->descriptor.raw.in_endpoint_count + fn->descriptor.raw.out_endpoint_count;
        state->uses_iad |= fn->descriptor.raw.uses_iad;
        break;

    default:
        return ESP_ERR_INVALID_ARG;
    }

    state->class_bitmap |= class_bit(fn->type, fn->instance);
    add_callback_slot(fn);
    return ESP_OK;
}

static esp_err_t build_config_desc(const tinyusb_runtime_config_t *runtime, bool high_speed,
                                   uint8_t attrs, uint16_t power_ma, uint8_t **out_buf,
                                   uint16_t *out_len, compose_state_t *final_state)
{
    esp_err_t ret;
    desc_builder_t builder;
    compose_state_t state = {
        .next_itf = 0,
        .next_ep = 1,
        .next_str = 4,
        .class_bitmap = 0,
        .uses_iad = false,
    };
    uint16_t cap = TUD_CONFIG_DESC_LEN + runtime->function_count * 128;
    ESP_RETURN_ON_ERROR(builder_init(&builder, cap), TAG, "Init descriptor builder");
    ESP_GOTO_ON_ERROR(builder_reserve_config(&builder), fail, TAG, "Reserve config descriptor");

    for (size_t i = 0; i < runtime->function_count; i++) {
        ESP_GOTO_ON_ERROR(append_function(&builder, &state, &runtime->functions[i], high_speed),
                          fail, TAG, "Append runtime function");
    }

    builder_finish_config(&builder, state.next_itf, attrs, power_ma);
    *out_buf = builder.buf;
    *out_len = builder.len;
    if (final_state) {
        *final_state = state;
    }
    return ESP_OK;

fail:
    free(builder.buf);
    return ret;
}

static void fill_device_descriptor(const tinyusb_runtime_config_t *runtime, const compose_state_t *state,
                                   tusb_desc_device_t *device)
{
    if (runtime->device) {
        *device = *runtime->device;
    } else {
        *device = descriptor_dev_default;
    }

    if (state->uses_iad) {
        device->bDeviceClass = TUSB_CLASS_MISC;
        device->bDeviceSubClass = MISC_SUBCLASS_COMMON;
        device->bDeviceProtocol = MISC_PROTOCOL_IAD;
    } else {
        device->bDeviceClass = 0;
        device->bDeviceSubClass = 0;
        device->bDeviceProtocol = 0;
    }

    if (runtime->id_product) {
        device->idProduct = runtime->id_product;
    } else {
        device->idProduct = RUNTIME_PID_BASE | (state->class_bitmap & 0x0fff);
    }

    if (runtime->bcd_device) {
        device->bcdDevice = runtime->bcd_device;
    }
}

static esp_err_t build_strings(const tinyusb_runtime_config_t *runtime, uint8_t min_count,
                               const char ***out_str, int *out_count)
{
    static const char *fallback = "USB Function";
    const char **src = runtime->string ? runtime->string : descriptor_str_default;
    int count = runtime->string_count;

    if (!runtime->string) {
        count = 0;
        while (descriptor_str_default[count] != NULL && count < USB_STRING_DESCRIPTOR_ARRAY_SIZE) {
            count++;
        }
    }

    if (count < min_count) {
        count = min_count;
    }

    ESP_RETURN_ON_FALSE(count <= USB_STRING_DESCRIPTOR_ARRAY_SIZE, ESP_ERR_NOT_SUPPORTED, TAG, "String descriptors exceed limit");
    const char **copy = calloc(USB_STRING_DESCRIPTOR_ARRAY_SIZE, sizeof(char *));
    ESP_RETURN_ON_FALSE(copy, ESP_ERR_NO_MEM, TAG, "No memory for strings");
    for (int i = 0; i < count; i++) {
        copy[i] = src[i] ? src[i] : fallback;
    }
    *out_str = copy;
    *out_count = count;
    return ESP_OK;
}

esp_err_t tinyusb_runtime_descriptor_build(tinyusb_port_t port,
                                           const tinyusb_runtime_config_t *runtime,
                                           tinyusb_runtime_desc_t *out_desc)
{
    esp_err_t ret;
    ESP_RETURN_ON_FALSE(runtime && out_desc, ESP_ERR_INVALID_ARG, TAG, "Invalid runtime descriptor args");
    ESP_RETURN_ON_FALSE(runtime->functions || runtime->function_count == 0, ESP_ERR_INVALID_ARG, TAG, "Runtime functions are NULL");

    memset(out_desc, 0, sizeof(*out_desc));
    memset(&s_build_cb_map, 0, sizeof(s_build_cb_map));
    s_build_net_mac_string_id = 0;

    const uint8_t attrs = runtime->attributes ? runtime->attributes : RUNTIME_DEFAULT_CONFIG_ATTR;
    const uint16_t power_ma = runtime->power_ma ? runtime->power_ma : RUNTIME_DEFAULT_POWER_MA;
    compose_state_t state = { 0 };

    uint8_t *fs_cfg = NULL;
    uint8_t *hs_cfg = NULL;
    uint16_t fs_len = 0;
    uint16_t hs_len = 0;
    const char **strings = NULL;
    int str_count = 0;
    tusb_desc_device_t *dev = NULL;
    tusb_desc_device_qualifier_t *qualifier = NULL;

    ESP_GOTO_ON_ERROR(build_config_desc(runtime, false, attrs, power_ma, &fs_cfg, &fs_len, &state),
                      fail, TAG, "Build FS descriptor");

#if (TUD_OPT_HIGH_SPEED)
    if (port_is_hs(port)) {
        ESP_GOTO_ON_ERROR(build_config_desc(runtime, true, attrs, power_ma, &hs_cfg, &hs_len, NULL),
                          fail, TAG, "Build HS descriptor");
    }
#else
    (void)port;
#endif

    dev = calloc(1, sizeof(tusb_desc_device_t));
    ESP_GOTO_ON_FALSE(dev, ESP_ERR_NO_MEM, fail, TAG, "No memory for device descriptor");
    fill_device_descriptor(runtime, &state, dev);

#if (TUD_OPT_HIGH_SPEED)
    if (hs_cfg) {
        qualifier = calloc(1, sizeof(tusb_desc_device_qualifier_t));
        ESP_GOTO_ON_FALSE(qualifier, ESP_ERR_NO_MEM, fail, TAG, "No memory for qualifier");
        *qualifier = descriptor_qualifier_default;
        qualifier->bDeviceClass = dev->bDeviceClass;
        qualifier->bDeviceSubClass = dev->bDeviceSubClass;
        qualifier->bDeviceProtocol = dev->bDeviceProtocol;
    }
#endif

    ESP_GOTO_ON_ERROR(build_strings(runtime, state.next_str, &strings, &str_count), fail, TAG, "Build string table");

    out_desc->desc_cfg = (tinyusb_desc_config_t) {
        .device = dev,
        .qualifier = qualifier,
        .string = strings,
        .string_count = str_count,
        .full_speed_config = fs_cfg,
        .high_speed_config = hs_cfg,
    };
    out_desc->owns_desc = true;
    s_cb_map = s_build_cb_map;
    s_net_mac_string_id = s_build_net_mac_string_id;
    memset(&s_build_cb_map, 0, sizeof(s_build_cb_map));
    s_build_net_mac_string_id = 0;
    (void)fs_len;
    (void)hs_len;
    return ESP_OK;

fail:
    free(fs_cfg);
    free(hs_cfg);
    free(dev);
    free(qualifier);
    free(strings);
    memset(&s_build_cb_map, 0, sizeof(s_build_cb_map));
    s_build_net_mac_string_id = 0;
    return ret;
}

void tinyusb_runtime_descriptor_free(tinyusb_runtime_desc_t *desc)
{
    if (!desc || !desc->owns_desc) {
        return;
    }
    free((void *)desc->desc_cfg.device);
    free((void *)desc->desc_cfg.qualifier);
    free((void *)desc->desc_cfg.string);
    free((void *)desc->desc_cfg.full_speed_config);
    free((void *)desc->desc_cfg.high_speed_config);
    memset(desc, 0, sizeof(*desc));
}

esp_err_t tinyusb_runtime_activate(tinyusb_port_t port,
                                   const tinyusb_runtime_config_t *runtime)
{
    tinyusb_runtime_desc_t desc;
    ESP_RETURN_ON_ERROR(tinyusb_runtime_descriptor_build(port, runtime, &desc), TAG, "Build runtime descriptor");
    esp_err_t ret = tinyusb_descriptors_set(port, &desc.desc_cfg);
    tinyusb_runtime_descriptor_free(&desc);
    if (ret == ESP_OK) {
        s_runtime_active = true;
    }
    return ret;
}

void tinyusb_runtime_deactivate(void)
{
    memset(&s_cb_map, 0, sizeof(s_cb_map));
    memset(&s_build_cb_map, 0, sizeof(s_build_cb_map));
    s_runtime_active = false;
    s_net_mac_string_id = 0;
    s_build_net_mac_string_id = 0;
}

bool tinyusb_runtime_is_active(void)
{
    return s_runtime_active;
}

void tinyusb_runtime_set_active(bool active)
{
    s_runtime_active = active;
    if (!active) {
        memset(&s_cb_map, 0, sizeof(s_cb_map));
        memset(&s_build_cb_map, 0, sizeof(s_build_cb_map));
        s_net_mac_string_id = 0;
        s_build_net_mac_string_id = 0;
    }
}

void *tinyusb_runtime_get_callback_ctx(tinyusb_runtime_class_t type, uint8_t instance)
{
    for (size_t i = 0; i < s_cb_map.slot_count; i++) {
        if (s_cb_map.slots[i].type == type && s_cb_map.slots[i].instance == instance) {
            return s_cb_map.slots[i].ctx;
        }
    }
    return NULL;
}

bool tinyusb_runtime_has_class(tinyusb_runtime_class_t type)
{
    for (size_t i = 0; i < s_cb_map.slot_count; i++) {
        if (s_cb_map.slots[i].type == type) {
            return true;
        }
    }
    return false;
}

uint8_t tinyusb_runtime_get_net_mac_string_id(void)
{
    return s_net_mac_string_id;
}
