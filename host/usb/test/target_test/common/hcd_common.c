/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_intr_alloc.h"
#include "esp_err.h"
#include "esp_attr.h"
#include "esp_rom_gpio.h"
#include "usb_private.h"
#include "usb/usb_types_ch9.h"
#include "esp_private/usb_phy.h"
#include "hcd_common.h"
#include "mock_msc.h"
#include "phy_common.h"
#include "unity.h"
#include "sdkconfig.h"

// ----------------------------------------------------- Macros --------------------------------------------------------

#define EVENT_QUEUE_LEN         5
#define ENUM_ADDR               1   // Device address to use for tests that enumerate the device
#define ENUM_CONFIG             1   // Device configuration number to use for tests that enumerate the device
#define TRANSFER_MAX_BYTES      256

typedef struct {
    hcd_port_handle_t port_hdl;
    hcd_port_event_t port_event;
} port_event_msg_t;

typedef struct {
    hcd_pipe_handle_t pipe_hdl;
    hcd_pipe_event_t pipe_event;
} pipe_event_msg_t;

hcd_port_handle_t port_hdl = NULL;

// ----------------------------------------------------- Logging -------------------------------------------------------

// Buffer for building Unity failure messages that include the caller's file and line
static char err_msg_buf[128];

// Human-readable names for HCD port events, indexed by hcd_port_event_t
static const char *const port_event_names[] = {
    [HCD_PORT_EVENT_NONE]           = "HCD_PORT_EVENT_NONE",
    [HCD_PORT_EVENT_CONNECTION]     = "HCD_PORT_EVENT_CONNECTION",
    [HCD_PORT_EVENT_DISCONNECTION]  = "HCD_PORT_EVENT_DISCONNECTION",
    [HCD_PORT_EVENT_ERROR]          = "HCD_PORT_EVENT_ERROR",
    [HCD_PORT_EVENT_OVERCURRENT]    = "HCD_PORT_EVENT_OVERCURRENT",
    [HCD_PORT_EVENT_REMOTE_WAKEUP]  = "HCD_PORT_EVENT_REMOTE_WAKEUP",
};

// Human-readable names for HCD pipe events, indexed by hcd_pipe_event_t
static const char *const pipe_event_names[] = {
    [HCD_PIPE_EVENT_NONE]                   = "HCD_PIPE_EVENT_NONE",
    [HCD_PIPE_EVENT_URB_DONE]               = "HCD_PIPE_EVENT_URB_DONE",
    [HCD_PIPE_EVENT_ERROR_XFER]             = "HCD_PIPE_EVENT_ERROR_XFER",
    [HCD_PIPE_EVENT_ERROR_URB_NOT_AVAIL]    = "HCD_PIPE_EVENT_ERROR_URB_NOT_AVAIL",
    [HCD_PIPE_EVENT_ERROR_OVERFLOW]         = "HCD_PIPE_EVENT_ERROR_OVERFLOW",
    [HCD_PIPE_EVENT_ERROR_STALL]            = "HCD_PIPE_EVENT_ERROR_STALL",
};

// Human-readable names for HCD port states, indexed by hcd_port_state_t
static const char *const port_state_names[] = {
    [HCD_PORT_STATE_NOT_POWERED]    = "HCD_PORT_STATE_NOT_POWERED",
    [HCD_PORT_STATE_DISCONNECTED]   = "HCD_PORT_STATE_DISCONNECTED",
    [HCD_PORT_STATE_DISABLED]       = "HCD_PORT_STATE_DISABLED",
    [HCD_PORT_STATE_RESETTING]      = "HCD_PORT_STATE_RESETTING",
    [HCD_PORT_STATE_SUSPENDING]     = "HCD_PORT_STATE_SUSPENDING",
    [HCD_PORT_STATE_SUSPENDED]      = "HCD_PORT_STATE_SUSPENDED",
    [HCD_PORT_STATE_RESUMING]       = "HCD_PORT_STATE_RESUMING",
    [HCD_PORT_STATE_ENABLED]        = "HCD_PORT_STATE_ENABLED",
    [HCD_PORT_STATE_RECOVERY]       = "HCD_PORT_STATE_RECOVERY",
};

// Human-readable names for HCD pipe states, indexed by hcd_pipe_state_t
static const char *const pipe_state_names[] = {
    [HCD_PIPE_STATE_ACTIVE]         = "HCD_PIPE_STATE_ACTIVE",
    [HCD_PIPE_STATE_HALTED]         = "HCD_PIPE_STATE_HALTED",
};

// Human-readable names for USB transfer statuses, indexed by usb_transfer_status_t
static const char *const transfer_status_names[] = {
    [USB_TRANSFER_STATUS_COMPLETED] = "USB_TRANSFER_STATUS_COMPLETED",
    [USB_TRANSFER_STATUS_ERROR]     = "USB_TRANSFER_STATUS_ERROR",
    [USB_TRANSFER_STATUS_TIMED_OUT] = "USB_TRANSFER_STATUS_TIMED_OUT",
    [USB_TRANSFER_STATUS_CANCELED]  = "USB_TRANSFER_STATUS_CANCELED",
    [USB_TRANSFER_STATUS_STALL]     = "USB_TRANSFER_STATUS_STALL",
    [USB_TRANSFER_STATUS_OVERFLOW]  = "USB_TRANSFER_STATUS_OVERFLOW",
    [USB_TRANSFER_STATUS_SKIPPED]   = "USB_TRANSFER_STATUS_SKIPPED",
    [USB_TRANSFER_STATUS_NO_DEVICE] = "USB_TRANSFER_STATUS_NO_DEVICE",
};

/**
 * @brief Look up a name in a name table
 *
 * @param names Name table indexed by enum value
 * @param count Number of entries in the name table
 * @param value Enum value to resolve
 * @return const char* Matching name, or "UNKNOWN" for out-of-range indices or gaps in the table
 */
static const char *enum_to_str(const char *const *names, size_t count, int value)
{
    if (value < 0 || (size_t)value >= count || names[value] == NULL) {
        return "UNKNOWN";
    }
    return names[value];
}

// Resolve an enum value against its name table (which must be in scope)
#define ENUM_TO_STR(names, value) enum_to_str((names), sizeof(names) / sizeof((names)[0]), (value))

/**
 * @brief Get the human-readable name of an HCD port event
 */
static const char *port_event_str(hcd_port_event_t event)
{
    return ENUM_TO_STR(port_event_names, event);
}

/**
 * @brief Get the human-readable name of an HCD pipe event
 */
static const char *pipe_event_str(hcd_pipe_event_t event)
{
    return ENUM_TO_STR(pipe_event_names, event);
}

/**
 * @brief Get the human-readable name of an HCD port state
 */
static const char *port_state_str(hcd_port_state_t state)
{
    return ENUM_TO_STR(port_state_names, state);
}

/**
 * @brief Get the human-readable name of an HCD pipe state
 */
static const char *pipe_state_str(hcd_pipe_state_t state)
{
    return ENUM_TO_STR(pipe_state_names, state);
}

/**
 * @brief Get the human-readable name of a USB transfer status
 */
static const char *transfer_status_str(usb_transfer_status_t status)
{
    return ENUM_TO_STR(transfer_status_names, status);
}

// ---------------------------------------------------- Private --------------------------------------------------------

/**
 * @brief HCD port callback. Registered when initializing an HCD port
 *
 * @param port_hdl Port handle
 * @param port_event Port event that triggered the callback
 * @param user_arg User argument
 * @param in_isr Whether callback was called in an ISR context
 * @return true ISR should yield after this callback returns
 * @return false No yield required (non-ISR context calls should always return false)
 */
static bool port_callback(hcd_port_handle_t port_hdl, hcd_port_event_t port_event, void *user_arg, bool in_isr)
{
    // We store the port's queue handle in the port's context variable
    void *port_ctx = hcd_port_get_context(port_hdl);
    QueueHandle_t port_evt_queue = (QueueHandle_t)port_ctx;
    TEST_ASSERT_TRUE(in_isr);    // Current HCD implementation should never call a port callback in a task context
    port_event_msg_t msg = {
        .port_hdl = port_hdl,
        .port_event = port_event,
    };
    BaseType_t xTaskWoken = pdFALSE;
    BaseType_t ret = xQueueSendFromISR(port_evt_queue, &msg, &xTaskWoken);
    // Asserting in case of the queue being full instead of TEST_ASSERT which is not ISR safe function
    configASSERT(ret == pdTRUE);
    return (xTaskWoken == pdTRUE);
}

/**
 * @brief HCD pipe callback. Registered when allocating a HCD pipe
 *
 * @param pipe_hdl Pipe handle
 * @param pipe_event Pipe event that triggered the callback
 * @param user_arg User argument
 * @param in_isr Whether the callback was called in an ISR context
 * @return true ISR should yield after this callback returns
 * @return false No yield required (non-ISR context calls should always return false)
 */
static bool pipe_callback(hcd_pipe_handle_t pipe_hdl, hcd_pipe_event_t pipe_event, void *user_arg, bool in_isr)
{
    QueueHandle_t pipe_evt_queue = (QueueHandle_t)user_arg;
    pipe_event_msg_t msg = {
        .pipe_hdl = pipe_hdl,
        .pipe_event = pipe_event,
    };
    if (in_isr) {
        BaseType_t xTaskWoken = pdFALSE;
        BaseType_t ret = xQueueSendFromISR(pipe_evt_queue, &msg, &xTaskWoken);
        // Asserting in case of the queue being full instead of TEST_ASSERT which is not ISR safe function
        configASSERT(ret == pdTRUE);
        return (xTaskWoken == pdTRUE);
    } else {
        BaseType_t ret = xQueueSend(pipe_evt_queue, &msg, pdMS_TO_TICKS(10000));
        TEST_ASSERT_EQUAL_MESSAGE(pdTRUE, ret, "Pipe event queue full, event lost");
        return false;
    }
}

// ------------------------------------------------- HCD Event Test ----------------------------------------------------

void test_hcd_expect_port_event_impl(hcd_port_handle_t port_hdl, hcd_port_event_t expected_event, const char *file, int line)
{
    // Get the port event queue from the port's context variable
    QueueHandle_t port_evt_queue = (QueueHandle_t)hcd_port_get_context(port_hdl);
    TEST_ASSERT_NOT_NULL(port_evt_queue);
    // Wait for port callback to send an event message
    port_event_msg_t msg;
    BaseType_t ret = xQueueReceive(port_evt_queue, &msg, pdMS_TO_TICKS(5000));
    if (ret != pdPASS) {
        snprintf(err_msg_buf, sizeof(err_msg_buf), "Port event %s not generated on time at %s:%d", port_event_str(expected_event), file, line);
        TEST_FAIL_MESSAGE(err_msg_buf);
    }
    // Check the contents of that event message
    TEST_ASSERT_EQUAL(port_hdl, msg.port_hdl);
    if (expected_event != msg.port_event) {
        snprintf(err_msg_buf, sizeof(err_msg_buf), "Unexpected port event at %s:%d\n %s expected, %s delivered", file, line, port_event_str(expected_event), port_event_str(msg.port_event));
        TEST_FAIL_MESSAGE(err_msg_buf);
    }
    printf("\t-> Port event\n");
}

void test_hcd_expect_pipe_event_impl(hcd_pipe_handle_t pipe_hdl, hcd_pipe_event_t expected_event, const char *file, int line)
{
    // Get the pipe's event queue from the pipe's context variable
    QueueHandle_t pipe_evt_queue = (QueueHandle_t)hcd_pipe_get_context(pipe_hdl);
    TEST_ASSERT_NOT_NULL(pipe_evt_queue);
    // Wait for pipe callback to send an event message
    pipe_event_msg_t msg;
    BaseType_t ret =  xQueueReceive(pipe_evt_queue, &msg, pdMS_TO_TICKS(5000));
    if (ret != pdPASS) {
        snprintf(err_msg_buf, sizeof(err_msg_buf), "Pipe event %s not generated on time at %s:%d", pipe_event_str(expected_event), file, line);
        TEST_FAIL_MESSAGE(err_msg_buf);
    }
    // Check the contents of that event message
    TEST_ASSERT_EQUAL(pipe_hdl, msg.pipe_hdl);
    if (expected_event != msg.pipe_event) {
        snprintf(err_msg_buf, sizeof(err_msg_buf), "Unexpected pipe event at %s:%d\n %s expected, %s delivered", file, line, pipe_event_str(expected_event), pipe_event_str(msg.pipe_event));
        TEST_FAIL_MESSAGE(err_msg_buf);
    }
}

void test_hcd_expect_no_pipe_event_impl(hcd_pipe_handle_t pipe_hdl, const char *file, int line)
{
    // Get the pipe's event queue from the pipe's context variable
    QueueHandle_t pipe_evt_queue = (QueueHandle_t)hcd_pipe_get_context(pipe_hdl);
    TEST_ASSERT_NOT_NULL(pipe_evt_queue);
    // Wait for pipe callback to send an event message
    pipe_event_msg_t msg;
    BaseType_t ret =  xQueueReceive(pipe_evt_queue, &msg, pdMS_TO_TICKS(2000));
    if (ret != pdFALSE) {
        snprintf(err_msg_buf, sizeof(err_msg_buf), "Expecting NO pipe event, but %s delivered at %s:%d", pipe_event_str(msg.pipe_event), file, line);
        TEST_FAIL_MESSAGE(err_msg_buf);
    }
}

void test_hcd_expect_port_state_impl(hcd_port_handle_t port_hdl, hcd_port_state_t expected_state, const char *file, int line)
{
    hcd_port_state_t actual_state = hcd_port_get_state(port_hdl);
    if (actual_state != expected_state) {
        snprintf(err_msg_buf, sizeof(err_msg_buf), "Unexpected port state at %s:%d\n %s expected, %s actual", file, line, port_state_str(expected_state), port_state_str(actual_state));
        TEST_FAIL_MESSAGE(err_msg_buf);
    }
}

void test_hcd_expect_pipe_state_impl(hcd_pipe_handle_t pipe_hdl, hcd_pipe_state_t expected_state, const char *file, int line)
{
    hcd_pipe_state_t actual_state = hcd_pipe_get_state(pipe_hdl);
    if (actual_state != expected_state) {
        snprintf(err_msg_buf, sizeof(err_msg_buf), "Unexpected pipe state at %s:%d\n %s expected, %s actual", file, line, pipe_state_str(expected_state), pipe_state_str(actual_state));
        TEST_FAIL_MESSAGE(err_msg_buf);
    }
}

void test_hcd_expect_transfer_status_impl(const urb_t *urb, usb_transfer_status_t expected_status, const char *file, int line)
{
    TEST_ASSERT_NOT_NULL(urb);
    if (urb->transfer.status != expected_status) {
        snprintf(err_msg_buf, sizeof(err_msg_buf), "Unexpected transfer status at %s:%d\n %s expected, %s delivered", file, line, transfer_status_str(expected_status), transfer_status_str(urb->transfer.status));
        TEST_FAIL_MESSAGE(err_msg_buf);
    }
}

int test_hcd_get_num_port_events(hcd_port_handle_t port_hdl)
{
    // Get the port event queue from the port's context variable
    QueueHandle_t port_evt_queue = (QueueHandle_t)hcd_port_get_context(port_hdl);
    TEST_ASSERT_NOT_NULL(port_evt_queue);
    return EVENT_QUEUE_LEN - uxQueueSpacesAvailable(port_evt_queue);
}

int test_hcd_get_num_pipe_events(hcd_pipe_handle_t pipe_hdl)
{
    // Get the pipe's event queue from the pipe's context variable
    QueueHandle_t pipe_evt_queue = (QueueHandle_t)hcd_pipe_get_context(pipe_hdl);
    TEST_ASSERT_NOT_NULL(pipe_evt_queue);
    return EVENT_QUEUE_LEN - uxQueueSpacesAvailable(pipe_evt_queue);
}

// ----------------------------------------------- Driver/Port Related -------------------------------------------------

hcd_port_handle_t test_hcd_setup(void)
{
    test_setup_usb_phy(TEST_PHY);

    // Create a queue for port callback to queue up port events
    QueueHandle_t port_evt_queue = xQueueCreate(EVENT_QUEUE_LEN, sizeof(port_event_msg_t));
    TEST_ASSERT_NOT_NULL(port_evt_queue);
    // Initialize a port
    hcd_port_config_t port_config = {
        .callback = port_callback,
        .callback_arg = (void *)port_evt_queue,
        .context = (void *)port_evt_queue,
        .fifo_config = NULL, // Default: use bias strategy from Kconfig
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    hcd_port_handle_t port_hdl;
    TEST_ASSERT_EQUAL(ESP_OK, hcd_port_init(TEST_PORT_NUM, &port_config, &port_hdl));
    TEST_ASSERT_NOT_NULL(port_hdl);
    return port_hdl;
}

void test_hcd_teardown(hcd_port_handle_t port_hdl)
{
    if (!port_hdl) {
        return; // In case of setup stage failure, don't run tear-down stage
    }
    // Get the queue handle from the port's context variable
    QueueHandle_t port_evt_queue = (QueueHandle_t)hcd_port_get_context(port_hdl);
    TEST_ASSERT_NOT_NULL(port_evt_queue);
    // Deinitialize a port
    TEST_ASSERT_EQUAL(ESP_OK, hcd_port_deinit(port_hdl));
    vQueueDelete(port_evt_queue);
    // Deinitialize the internal USB PHY
    test_delete_usb_phy();
}

usb_speed_t test_hcd_wait_for_conn(hcd_port_handle_t port_hdl)
{
    // Power ON the port. This should allow for connections to occur
    TEST_ASSERT_EQUAL(ESP_OK, hcd_port_command(port_hdl, HCD_PORT_CMD_POWER_ON));
    TEST_HCD_EXPECT_PORT_STATE(port_hdl, HCD_PORT_STATE_DISCONNECTED);
    // Wait for connection event
    printf("Waiting for connection\n");
    TEST_HCD_EXPECT_PORT_EVENT(port_hdl, HCD_PORT_EVENT_CONNECTION);
    TEST_ASSERT_EQUAL(HCD_PORT_EVENT_CONNECTION, hcd_port_handle_event(port_hdl));
    TEST_HCD_EXPECT_PORT_STATE(port_hdl, HCD_PORT_STATE_DISABLED);
    // Reset newly connected device
    printf("Resetting\n");
    TEST_ASSERT_EQUAL(ESP_OK, hcd_port_command(port_hdl, HCD_PORT_CMD_RESET));
    TEST_HCD_EXPECT_PORT_STATE(port_hdl, HCD_PORT_STATE_ENABLED);
    // Get speed of connected
    usb_speed_t port_speed;
    TEST_ASSERT_EQUAL(ESP_OK, hcd_port_get_speed(port_hdl, &port_speed));
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(USB_SPEED_HIGH, port_speed, "Invalid port speed");
    printf("%s speed enabled\n", (char *[]) {
        "Low", "Full", "High"
    }[port_speed]);
    return port_speed;
}

void test_hcd_wait_for_disconn(hcd_port_handle_t port_hdl, bool already_disabled)
{
    if (!already_disabled) {
        // Disable the device
        printf("Disabling\n");
        TEST_ASSERT_EQUAL(ESP_OK, hcd_port_command(port_hdl, HCD_PORT_CMD_DISABLE));
        TEST_HCD_EXPECT_PORT_STATE(port_hdl, HCD_PORT_STATE_DISABLED);
    }
    printf("Waiting for disconnection\n");
    // Power-off the port to trigger a disconnection
    TEST_ASSERT_EQUAL(ESP_OK, hcd_port_command(port_hdl, HCD_PORT_CMD_POWER_OFF));
    // Wait for the port disconnection event
    TEST_HCD_EXPECT_PORT_EVENT(port_hdl, HCD_PORT_EVENT_DISCONNECTION);
    TEST_ASSERT_EQUAL(HCD_PORT_EVENT_DISCONNECTION, hcd_port_handle_event(port_hdl));
    TEST_HCD_EXPECT_PORT_STATE(port_hdl, HCD_PORT_STATE_RECOVERY);
    // Power down the port
    TEST_ASSERT_EQUAL(ESP_OK, hcd_port_command(port_hdl, HCD_PORT_CMD_POWER_OFF));
    TEST_HCD_EXPECT_PORT_STATE(port_hdl, HCD_PORT_STATE_NOT_POWERED);
}

void test_hcd_root_port_suspend(hcd_port_handle_t port_hdl, hcd_pipe_handle_t pipe_hdl)
{
    // Halt and flush the pipe
    TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_command(pipe_hdl, HCD_PIPE_CMD_HALT));
    TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_command(pipe_hdl, HCD_PIPE_CMD_FLUSH));
    TEST_HCD_EXPECT_PIPE_STATE(pipe_hdl, HCD_PIPE_STATE_HALTED);
    // Suspend the root port
    TEST_ASSERT_EQUAL(ESP_OK, hcd_port_command(port_hdl, HCD_PORT_CMD_SUSPEND));
    TEST_HCD_EXPECT_PORT_STATE(port_hdl, HCD_PORT_STATE_SUSPENDED);
    printf("Root port suspended\n");
}

void test_hcd_root_port_suspend_multi_pipe(hcd_port_handle_t port_hdl, hcd_pipe_handle_t *pipe_list, int list_len)
{
    for (int pipe_i = 0; pipe_i < list_len; pipe_i++) {
        hcd_pipe_handle_t pipe_hdl = pipe_list[pipe_i];
        // Halt and flush the pipe
        TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_command(pipe_hdl, HCD_PIPE_CMD_HALT));
        TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_command(pipe_hdl, HCD_PIPE_CMD_FLUSH));
        TEST_HCD_EXPECT_PIPE_STATE(pipe_hdl, HCD_PIPE_STATE_HALTED);
    }

    // Suspend the root port
    TEST_ASSERT_EQUAL(ESP_OK, hcd_port_command(port_hdl, HCD_PORT_CMD_SUSPEND));
    TEST_HCD_EXPECT_PORT_STATE(port_hdl, HCD_PORT_STATE_SUSPENDED);
    printf("Root port suspended\n");
}

void test_hcd_root_port_resume(hcd_port_handle_t port_hdl, hcd_pipe_handle_t pipe_hdl)
{
    // Resume the root port
    TEST_ASSERT_EQUAL(ESP_OK, hcd_port_command(port_hdl, HCD_PORT_CMD_RESUME));
    TEST_HCD_EXPECT_PORT_STATE(port_hdl, HCD_PORT_STATE_ENABLED);
    // Clear the pipe
    TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_command(pipe_hdl, HCD_PIPE_CMD_CLEAR));
    TEST_HCD_EXPECT_PIPE_STATE(pipe_hdl, HCD_PIPE_STATE_ACTIVE);
    printf("Root port resumed\n");
}

void test_hcd_root_port_resume_multi_pipe(hcd_port_handle_t port_hdl, hcd_pipe_handle_t *pipe_list, int list_len)
{
    // Resume the root port
    TEST_ASSERT_EQUAL(ESP_OK, hcd_port_command(port_hdl, HCD_PORT_CMD_RESUME));
    TEST_HCD_EXPECT_PORT_STATE(port_hdl, HCD_PORT_STATE_ENABLED);

    for (int pipe_i = 0; pipe_i < list_len; pipe_i++) {
        hcd_pipe_handle_t pipe_hdl = pipe_list[pipe_i];
        // Clear the pipe
        TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_command(pipe_hdl, HCD_PIPE_CMD_CLEAR));
        TEST_HCD_EXPECT_PIPE_STATE(pipe_hdl, HCD_PIPE_STATE_ACTIVE);
    }
}

// ---------------------------------------------- Pipe Setup/Tear-down -------------------------------------------------

hcd_pipe_handle_t test_hcd_pipe_alloc(hcd_port_handle_t port_hdl, const usb_ep_desc_t *ep_desc, uint8_t dev_addr, usb_speed_t dev_speed)
{
    // Create a queue for pipe callback to queue up pipe events
    QueueHandle_t pipe_evt_queue = xQueueCreate(EVENT_QUEUE_LEN, sizeof(pipe_event_msg_t));
    TEST_ASSERT_NOT_NULL(pipe_evt_queue);
    hcd_pipe_config_t pipe_config = {
        .callback = pipe_callback,
        .callback_arg = (void *)pipe_evt_queue,
        .context = (void *)pipe_evt_queue,
        .ep_desc = ep_desc,
        .dev_addr = dev_addr,
        .dev_speed = dev_speed,
    };
    hcd_pipe_handle_t pipe_hdl;
    TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_alloc(port_hdl, &pipe_config, &pipe_hdl));
    TEST_ASSERT_NOT_NULL(pipe_hdl);
    return pipe_hdl;
}

void test_hcd_pipe_free(hcd_pipe_handle_t pipe_hdl)
{
    // Get the pipe's event queue from its context variable
    QueueHandle_t pipe_evt_queue = (QueueHandle_t)hcd_pipe_get_context(pipe_hdl);
    TEST_ASSERT_NOT_NULL(pipe_evt_queue);
    // Free the pipe and queue
    TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_free(pipe_hdl));
    vQueueDelete(pipe_evt_queue);
}

#include "esp_private/esp_cache_private.h"

#define ALIGN_UP(num, align)    ((align) == 0 ? (num) : (((num) + ((align) - 1)) & ~((align) - 1)))

#ifdef CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM      // In esp32p4, the USB-DWC internal DMA can access external RAM
#define DATA_BUFFER_CAPS                     (MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_SPIRAM)
#else
#define DATA_BUFFER_CAPS                     (MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_INTERNAL)
#endif

urb_t *test_hcd_alloc_urb(int num_isoc_packets, size_t data_buffer_size)
{
    // Allocate a URB and data buffer
    urb_t *urb = heap_caps_calloc(1, sizeof(urb_t) + (sizeof(usb_isoc_packet_desc_t) * num_isoc_packets), MALLOC_CAP_DEFAULT);

    size_t cache_align = 0;
    esp_cache_get_alignment(DATA_BUFFER_CAPS, &cache_align);
    data_buffer_size = ALIGN_UP(data_buffer_size, cache_align);
    void *data_buffer = heap_caps_malloc(data_buffer_size, DATA_BUFFER_CAPS);

    TEST_ASSERT_NOT_NULL_MESSAGE(urb, "Failed to allocate URB");
    TEST_ASSERT_NOT_NULL_MESSAGE(data_buffer, "Failed to allocate transfer buffer");

    // Initialize URB and underlying transfer structure. Need to cast to dummy due to const fields
    usb_transfer_dummy_t *transfer_dummy = (usb_transfer_dummy_t *)&urb->transfer;
    transfer_dummy->data_buffer = data_buffer;
    transfer_dummy->data_buffer_size = data_buffer_size;
    transfer_dummy->num_isoc_packets = num_isoc_packets;
    return urb;
}

void test_hcd_free_urb(urb_t *urb)
{
    // Free data buffer of the transfer
    heap_caps_free(urb->transfer.data_buffer);
    // Free the URB
    heap_caps_free(urb);
}

// --------------------------------------------------- Enumeration -----------------------------------------------------

uint8_t test_hcd_enum_device(hcd_pipe_handle_t default_pipe)
{
    // We need to create a URB for the enumeration control transfers
    urb_t *urb = test_hcd_alloc_urb(0, sizeof(usb_setup_packet_t) + 256);
    usb_setup_packet_t *setup_pkt = (usb_setup_packet_t *)urb->transfer.data_buffer;

    // Get the device descriptor (note that device might only return 8 bytes)
    USB_SETUP_PACKET_INIT_GET_DEVICE_DESC(setup_pkt);
    urb->transfer.num_bytes = sizeof(usb_setup_packet_t) + sizeof(usb_device_desc_t);
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(default_pipe, urb));
    TEST_HCD_EXPECT_PIPE_EVENT(default_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL(urb, hcd_urb_dequeue(default_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_COMPLETED);

    // Update the MPS of the default pipe
    usb_device_desc_t *device_desc = (usb_device_desc_t *)(urb->transfer.data_buffer + sizeof(usb_setup_packet_t));
    TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_update_mps(default_pipe, device_desc->bMaxPacketSize0));

    // Send a set address request
    USB_SETUP_PACKET_INIT_SET_ADDR(setup_pkt, ENUM_ADDR);    // We only support one device for now so use address 1
    urb->transfer.num_bytes = sizeof(usb_setup_packet_t);
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(default_pipe, urb));
    TEST_HCD_EXPECT_PIPE_EVENT(default_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL(urb, hcd_urb_dequeue(default_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_COMPLETED);

    // Update address of default pipe
    TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_update_dev_addr(default_pipe, ENUM_ADDR));

    // Some high-speed Hubs need some time before being able to process SetConfiguration request
    // Full-speed devices doesn't require that time
    vTaskDelay(pdMS_TO_TICKS(10));

    // Send a set configuration request
    USB_SETUP_PACKET_INIT_SET_CONFIG(setup_pkt, ENUM_CONFIG);
    urb->transfer.num_bytes = sizeof(usb_setup_packet_t);
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(default_pipe, urb));
    TEST_HCD_EXPECT_PIPE_EVENT(default_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL(urb, hcd_urb_dequeue(default_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_COMPLETED);

    // Free URB
    test_hcd_free_urb(urb);
    return ENUM_ADDR;
}

// ---------------------------------------------- Transfer submit ------------------------------------------------------

void test_hcd_ping_device(hcd_pipe_handle_t default_pipe, urb_t *default_urb)
{
    // Initialize the default URB with get config request
    default_urb->transfer.num_bytes = sizeof(usb_setup_packet_t) + TRANSFER_MAX_BYTES;
    USB_SETUP_PACKET_INIT_GET_CONFIG_DESC((usb_setup_packet_t *)default_urb->transfer.data_buffer, 0, TRANSFER_MAX_BYTES);
    default_urb->transfer.context = URB_CONTEXT_VAL;

    // Enqueue urb, wait for the URB_DONE event and dequeue urb
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(default_pipe, default_urb));
    TEST_HCD_EXPECT_PIPE_EVENT(default_pipe, HCD_PIPE_EVENT_URB_DONE);
    urb_t *urb = hcd_urb_dequeue(default_pipe);

    // Check the dequeued URB
    TEST_ASSERT_EQUAL_MESSAGE(default_urb, urb, "URB pointers not equal");
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_COMPLETED);
    TEST_ASSERT_EQUAL_MESSAGE(URB_CONTEXT_VAL, urb->transfer.context, "URB context not equal");

    // We must have transmitted at least the setup packet, but device may return less bytes than bytes requested
    TEST_ASSERT_GREATER_OR_EQUAL(sizeof(usb_setup_packet_t), urb->transfer.actual_num_bytes);
    TEST_ASSERT_LESS_OR_EQUAL(urb->transfer.num_bytes, urb->transfer.actual_num_bytes);
    usb_config_desc_t *config_desc = (usb_config_desc_t *)(urb->transfer.data_buffer + sizeof(usb_setup_packet_t));
    TEST_ASSERT_EQUAL(USB_B_DESCRIPTOR_TYPE_CONFIGURATION, config_desc->bDescriptorType);
}

void test_hcd_remote_wake_enable(hcd_pipe_handle_t default_pipe, urb_t *feature_urb, bool enable)
{
    feature_urb->transfer.num_bytes = sizeof(usb_setup_packet_t);
    feature_urb->transfer.context = URB_CONTEXT_VAL;

    if (enable) {
        printf("Enabling device remote wake-up\n");
        // Initialize feature_urb with the set feature request to enable remote wakeup
        USB_SETUP_PACKET_INIT_SET_FEATURE((usb_setup_packet_t *)feature_urb->transfer.data_buffer, DEVICE_REMOTE_WAKEUP);
    } else {
        printf("Disabling device remote wake-up\n");
        // Initialize feature_urb with the clear feature request to disable remote wakeup
        USB_SETUP_PACKET_INIT_CLEAR_FEATURE((usb_setup_packet_t *)feature_urb->transfer.data_buffer, DEVICE_REMOTE_WAKEUP);
    }

    // Enqueue urb, wait for the URB_DONE event and dequeue urb
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(default_pipe, feature_urb));
    TEST_HCD_EXPECT_PIPE_EVENT(default_pipe, HCD_PIPE_EVENT_URB_DONE);
    urb_t *urb = hcd_urb_dequeue(default_pipe);

    // Check the dequeued URB
    TEST_ASSERT_EQUAL_MESSAGE(feature_urb, urb, "URB pointers not equal");
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_COMPLETED);
    TEST_ASSERT_EQUAL_MESSAGE(URB_CONTEXT_VAL, urb->transfer.context, "URB context not equal");

    TEST_ASSERT_EQUAL(sizeof(usb_setup_packet_t), urb->transfer.actual_num_bytes);
    TEST_ASSERT_EQUAL(urb->transfer.num_bytes, urb->transfer.actual_num_bytes);
}

bool test_hcd_remote_wake_check(hcd_pipe_handle_t default_pipe, urb_t *get_status_urb)
{
    // Initialize get_status_urb with a Get status request USB_SETUP_PACKET_INIT_GET_STATUS
    get_status_urb->transfer.num_bytes = sizeof(usb_setup_packet_t) + sizeof(usb_device_status_t);
    USB_SETUP_PACKET_INIT_GET_STATUS((usb_setup_packet_t *)get_status_urb->transfer.data_buffer);
    get_status_urb->transfer.context = URB_CONTEXT_VAL;

    // Enqueue urb, wait for the URB_DONE event and dequeue urb
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(default_pipe, get_status_urb));
    TEST_HCD_EXPECT_PIPE_EVENT(default_pipe, HCD_PIPE_EVENT_URB_DONE);
    urb_t *urb = hcd_urb_dequeue(default_pipe);

    // Check the dequeued URB
    TEST_ASSERT_EQUAL_MESSAGE(get_status_urb, urb, "URB pointers not equal");
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_COMPLETED);
    TEST_ASSERT_EQUAL_MESSAGE(URB_CONTEXT_VAL, urb->transfer.context, "URB context not equal");

    TEST_ASSERT_EQUAL(sizeof(usb_setup_packet_t) + sizeof(usb_device_status_t), urb->transfer.actual_num_bytes);
    TEST_ASSERT_EQUAL(urb->transfer.num_bytes, urb->transfer.actual_num_bytes);

    // Get the device status out of the data buffer
    usb_device_status_t *device_status = (usb_device_status_t *)(urb->transfer.data_buffer + sizeof(usb_setup_packet_t));
    const bool remote_wake_enabled = device_status->remote_wakeup;
    printf("Remote wake-up is currently %s\n", ((remote_wake_enabled)) ? ("enabled") : ("disabled") );
    return remote_wake_enabled;
}
