/*
 * SPDX-FileCopyrightText: 2019 Ha Thach (tinyusb.org),
 * SPDX-FileContributor: 2020-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org),
 * Additions Copyright (c) 2020, Espressif Systems (Shanghai) PTE LTD
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#pragma once

#include "tusb_option.h"
#include "sdkconfig.h"
#include "esp_assert.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_TINYUSB_CDC_ENABLED
#   define CONFIG_TINYUSB_CDC_ENABLED 0
#endif

#ifndef CONFIG_TINYUSB_CDC_COUNT
#   define CONFIG_TINYUSB_CDC_COUNT 0
#endif

#ifndef CONFIG_TINYUSB_MSC_ENABLED
#   define CONFIG_TINYUSB_MSC_ENABLED 0
#endif

#ifndef CONFIG_TINYUSB_MTP_ENABLED
#   define CONFIG_TINYUSB_MTP_ENABLED 0
#endif

#ifndef CONFIG_TINYUSB_HID_COUNT
#   define CONFIG_TINYUSB_HID_COUNT 0
#endif

#ifndef CONFIG_TINYUSB_MIDI_COUNT
#   define CONFIG_TINYUSB_MIDI_COUNT 0
#endif

#ifndef CONFIG_TINYUSB_VENDOR_COUNT
#   define CONFIG_TINYUSB_VENDOR_COUNT 0
#endif

#ifndef CONFIG_TINYUSB_NET_MODE_ECM_RNDIS
#   define CONFIG_TINYUSB_NET_MODE_ECM_RNDIS 0
#endif

#ifndef CONFIG_TINYUSB_NET_MODE_NCM
#   define CONFIG_TINYUSB_NET_MODE_NCM 0
#endif

#ifndef CONFIG_TINYUSB_DFU_MODE_DFU
#   define CONFIG_TINYUSB_DFU_MODE_DFU 0
#endif

#ifndef CONFIG_TINYUSB_DFU_MODE_DFU_RUNTIME
#   define CONFIG_TINYUSB_DFU_MODE_DFU_RUNTIME 0
#endif

#ifndef CONFIG_TINYUSB_BTH_ENABLED
#   define CONFIG_TINYUSB_BTH_ENABLED 0
#   define CONFIG_TINYUSB_BTH_ISO_ALT_COUNT 0
#endif

#ifndef CONFIG_TINYUSB_DEBUG_LEVEL
#   define CONFIG_TINYUSB_DEBUG_LEVEL 0
#endif

#define CFG_TUD_ENABLED                 1       // TinyUSB Device enabled

#if (CONFIG_IDF_TARGET_ESP32P4) || (CONFIG_IDF_TARGET_ESP32S31)
#define CFG_TUD_MAX_SPEED               OPT_MODE_HIGH_SPEED
#else
#define CFG_TUD_MAX_SPEED               OPT_MODE_FULL_SPEED
#endif

// ------------------------------------------------------------------------
//                              DCD DWC2 Mode
// ------------------------------------------------------------------------
#define CFG_TUD_DWC2_SLAVE_ENABLE   1       // Enable Slave/IRQ by default

// ------------------------------------------------------------------------
//                              DMA & Cache
// ------------------------------------------------------------------------
#ifdef CONFIG_TINYUSB_MODE_DMA
// DMA Mode has a priority over Slave/IRQ mode and will be used if hardware supports it
#define CFG_TUD_DWC2_DMA_ENABLE     1       // Enable DMA

// DCache maintenance is only needed when the SoC actually reaches internal
// SRAM (where TinyUSB DMA buffers live) via the L1 cache. Just having an L1
// cache for flash/PSRAM (CONFIG_CACHE_L1_CACHE_LINE_SIZE != 0) is not enough;
// e.g. ESP32-S31 has L1 cache but internal SRAM is not routed through it, so
// dcache clean/invalidate calls on USB buffers must stay disabled there.
#if CONFIG_CACHE_L1_CACHE_LINE_SIZE && CONFIG_SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
// Enable dcd_dcache clean/invalidate/clean_invalidate calls
#   define CFG_TUD_MEM_DCACHE_ENABLE    1
#define CFG_TUD_MEM_DCACHE_LINE_SIZE    CONFIG_CACHE_L1_CACHE_LINE_SIZE
// NOTE: starting with esp-idf v5.3 there is specific attribute present: DRAM_DMA_ALIGNED_ATTR
#   define CFG_TUSB_MEM_SECTION         __attribute__((aligned(CONFIG_CACHE_L1_CACHE_LINE_SIZE))) DRAM_ATTR
#else
#   define CFG_TUD_MEM_DCACHE_ENABLE    0
#   define CFG_TUD_MEM_CACHE_ENABLE     0
#   define CFG_TUSB_MEM_SECTION         TU_ATTR_ALIGNED(4) DRAM_ATTR
#endif // CONFIG_CACHE_L1_CACHE_LINE_SIZE && CONFIG_SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
#endif // CONFIG_TINYUSB_MODE_DMA

#define CFG_TUSB_OS                 OPT_OS_FREERTOS

/* USB DMA on some MCUs can only access a specific SRAM region with restriction on alignment.
 * Tinyusb use follows macros to declare transferring memory so that they can be put
 * into those specific section.
 * e.g
 * - CFG_TUSB_MEM SECTION : __attribute__ (( section(".usb_ram") ))
 * - CFG_TUSB_MEM_ALIGN   : __attribute__ ((aligned(4)))
 */
#ifndef CFG_TUSB_MEM_SECTION
#   define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#   define CFG_TUSB_MEM_ALIGN       TU_ATTR_ALIGNED(4)
#endif

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE      64
#endif

// Debug Level
#define CFG_TUSB_DEBUG              CONFIG_TINYUSB_DEBUG_LEVEL
#define CFG_TUSB_DEBUG_PRINTF       esp_rom_printf // TinyUSB can print logs from ISR, so we must use esp_rom_printf()

// CDC FIFO size of TX and RX
#define CFG_TUD_CDC_RX_BUFSIZE      CONFIG_TINYUSB_CDC_RX_BUFSIZE
#define CFG_TUD_CDC_TX_BUFSIZE      CONFIG_TINYUSB_CDC_TX_BUFSIZE
#define CFG_TUD_CDC_EP_BUFSIZE      CONFIG_TINYUSB_CDC_EP_BUFSIZE

// MSC Buffer size of Device Mass storage
#define CFG_TUD_MSC_BUFSIZE         CONFIG_TINYUSB_MSC_BUFSIZE

// MTP buffer sizes and DeviceInfo capabilities
#define CFG_TUD_MTP_EP_BUFSIZE      CONFIG_TINYUSB_MTP_EP_BUFSIZE
#define CFG_TUD_MTP_EP_CONTROL_BUFSIZE 16
// Android direct file I/O lets libmtp/GVFS open MTP objects for editor-style writes.
#ifndef MTP_OP_ANDROID_GET_PARTIAL_OBJECT_64
#define MTP_OP_ANDROID_GET_PARTIAL_OBJECT_64   0x95C1U
#endif
#ifndef MTP_OP_ANDROID_SEND_PARTIAL_OBJECT
#define MTP_OP_ANDROID_SEND_PARTIAL_OBJECT     0x95C2U
#endif
#ifndef MTP_OP_ANDROID_TRUNCATE_OBJECT
#define MTP_OP_ANDROID_TRUNCATE_OBJECT         0x95C3U
#endif
#ifndef MTP_OP_ANDROID_BEGIN_EDIT_OBJECT
#define MTP_OP_ANDROID_BEGIN_EDIT_OBJECT       0x95C4U
#endif
#ifndef MTP_OP_ANDROID_END_EDIT_OBJECT
#define MTP_OP_ANDROID_END_EDIT_OBJECT         0x95C5U
#endif
#define CFG_TUD_MTP_DEVICEINFO_EXTENSIONS   "microsoft.com: 1.0; android.com: 1.0; "
#define CFG_TUD_MTP_DEVICEINFO_SUPPORTED_OPERATIONS \
    MTP_OP_GET_DEVICE_INFO, \
    MTP_OP_OPEN_SESSION, \
    MTP_OP_CLOSE_SESSION, \
    MTP_OP_GET_STORAGE_IDS, \
    MTP_OP_GET_STORAGE_INFO, \
    MTP_OP_GET_NUM_OBJECTS, \
    MTP_OP_GET_OBJECT_HANDLES, \
    MTP_OP_GET_OBJECT_INFO, \
    MTP_OP_GET_OBJECT, \
    MTP_OP_GET_PARTIAL_OBJECT, \
    MTP_OP_ANDROID_GET_PARTIAL_OBJECT_64, \
    MTP_OP_DELETE_OBJECT, \
    MTP_OP_SEND_OBJECT_INFO, \
    MTP_OP_SEND_OBJECT, \
    MTP_OP_ANDROID_BEGIN_EDIT_OBJECT, \
    MTP_OP_ANDROID_SEND_PARTIAL_OBJECT, \
    MTP_OP_ANDROID_TRUNCATE_OBJECT, \
    MTP_OP_ANDROID_END_EDIT_OBJECT, \
    MTP_OP_GET_DEVICE_PROP_DESC, \
    MTP_OP_GET_DEVICE_PROP_VALUE, \
    MTP_OP_GET_OBJECT_PROPS_SUPPORTED, \
    MTP_OP_GET_OBJECT_PROP_DESC, \
    MTP_OP_GET_OBJECT_PROP_VALUE, \
    MTP_OP_SET_OBJECT_PROP_VALUE, \
    MTP_OP_GET_OBJECT_PROP_LIST, \
    MTP_OP_GET_OBJECT_PROP_REFERENCES
#define CFG_TUD_MTP_DEVICEINFO_SUPPORTED_EVENTS \
    MTP_EVENT_OBJECT_ADDED, \
    MTP_EVENT_OBJECT_REMOVED, \
    MTP_EVENT_OBJECT_INFO_CHANGED, \
    MTP_EVENT_OBJECT_PROP_CHANGED
#define CFG_TUD_MTP_DEVICEINFO_SUPPORTED_DEVICE_PROPERTIES \
    MTP_DEV_PROP_DEVICE_FRIENDLY_NAME
#define CFG_TUD_MTP_DEVICEINFO_CAPTURE_FORMATS \
    MTP_OBJ_FORMAT_UNDEFINED, \
    MTP_OBJ_FORMAT_ASSOCIATION
#define CFG_TUD_MTP_DEVICEINFO_PLAYBACK_FORMATS \
    MTP_OBJ_FORMAT_UNDEFINED, \
    MTP_OBJ_FORMAT_ASSOCIATION, \
    MTP_OBJ_FORMAT_TEXT, \
    MTP_OBJ_FORMAT_PNG, \
    MTP_OBJ_FORMAT_EXIF_JPEG, \
    MTP_OBJ_FORMAT_MP3, \
    MTP_OBJ_FORMAT_WAV, \
    MTP_OBJ_FORMAT_MP4

// MIDI macros
#define CFG_TUD_MIDI_EP_BUFSIZE     64
#define CFG_TUD_MIDI_EPSIZE         CFG_TUD_MIDI_EP_BUFSIZE
#define CFG_TUD_MIDI_RX_BUFSIZE     64
#define CFG_TUD_MIDI_TX_BUFSIZE     64

// Vendor FIFO size of TX and RX
#define CFG_TUD_VENDOR_RX_BUFSIZE   CONFIG_TINYUSB_VENDOR_RX_BUFSIZE
#define CFG_TUD_VENDOR_TX_BUFSIZE   CONFIG_TINYUSB_VENDOR_TX_BUFSIZE
#define CFG_TUD_VENDOR_EPSIZE       CONFIG_TINYUSB_VENDOR_EPSIZE

#if (CFG_TUD_VENDOR > 0)
#if (CFG_TUD_MAX_SPEED == OPT_MODE_HIGH_SPEED)
#define EP_SIZE_VENDOR   512
#else
#define EP_SIZE_VENDOR   64
#endif
ESP_STATIC_ASSERT(CFG_TUD_VENDOR_EPSIZE >= EP_SIZE_VENDOR, "Vendor EP size must be at least 64 for FS and 512 for HS");
#if (CFG_TUD_VENDOR_RX_BUFSIZE > 0)
ESP_STATIC_ASSERT(CFG_TUD_VENDOR_RX_BUFSIZE >= EP_SIZE_VENDOR, "Vendor RX buffer size must be at least equal to EP size");
#endif
#if (CFG_TUD_VENDOR_TX_BUFSIZE > 0)
ESP_STATIC_ASSERT(CFG_TUD_VENDOR_TX_BUFSIZE >= EP_SIZE_VENDOR, "Vendor TX buffer size must be at least equal to EP size");
#endif
#endif

// DFU macros
#define CFG_TUD_DFU_XFER_BUFSIZE    CONFIG_TINYUSB_DFU_BUFSIZE

// Number of BTH ISO alternatives
#define CFG_TUD_BTH_ISO_ALT_COUNT   CONFIG_TINYUSB_BTH_ISO_ALT_COUNT

// Enabled device class driver
#define CFG_TUD_CDC                 CONFIG_TINYUSB_CDC_COUNT
#define CFG_TUD_MSC                 CONFIG_TINYUSB_MSC_ENABLED
#define CFG_TUD_MTP                 CONFIG_TINYUSB_MTP_ENABLED
#define CFG_TUD_HID                 CONFIG_TINYUSB_HID_COUNT
#define CFG_TUD_MIDI                CONFIG_TINYUSB_MIDI_COUNT
#define CFG_TUD_VENDOR              CONFIG_TINYUSB_VENDOR_COUNT
#define CFG_TUD_ECM_RNDIS           CONFIG_TINYUSB_NET_MODE_ECM_RNDIS
#define CFG_TUD_NCM                 CONFIG_TINYUSB_NET_MODE_NCM
#define CFG_TUD_DFU                 CONFIG_TINYUSB_DFU_MODE_DFU
#define CFG_TUD_DFU_RUNTIME         CONFIG_TINYUSB_DFU_MODE_DFU_RUNTIME
#define CFG_TUD_BTH                 CONFIG_TINYUSB_BTH_ENABLED

// NCM NET Mode NTB buffers configuration
#define CFG_TUD_NCM_OUT_NTB_N         CONFIG_TINYUSB_NCM_OUT_NTB_BUFFS_COUNT
#define CFG_TUD_NCM_IN_NTB_N          CONFIG_TINYUSB_NCM_IN_NTB_BUFFS_COUNT
#define CFG_TUD_NCM_OUT_NTB_MAX_SIZE  CONFIG_TINYUSB_NCM_OUT_NTB_BUFF_MAX_SIZE
#define CFG_TUD_NCM_IN_NTB_MAX_SIZE   CONFIG_TINYUSB_NCM_IN_NTB_BUFF_MAX_SIZE

#ifdef __cplusplus
}
#endif
