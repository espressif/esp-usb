/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

/**
 * @brief Remote wakeup client test parameters
 */
typedef struct {
    SemaphoreHandle_t dev_ready_smp;    /**< Device ready state semaphore */
} remote_wake_client_test_param_t;

/**
 * @brief Remote wake-up client task
 */
void remote_wake_client_async_task(void *arg);

/**
 * @brief Enable remote wakeup on the device
 * @note A control transfer (SET_FEATURE) is sent to device
 * @note This function must not be called from the same task as the client task, as a control transfer is sent
 */
void test_remote_wake_enable(void);

/**
 * @brief Disable remote wakeup on the device
 * @note A control transfer (CLEAR_FEATURE) is sent to device
 * @note This function must not be called from the same task as the client task, as a control transfer is sent
 */
void test_remote_wake_disable(void);

/**
 * @brief Get current remote wakeup state from the device
 * @note A control transfer (GET_STATUS) is sent to device
 * @note This function must not be called from the same task as the client task, as a control transfer is sent
 *
 * @param[in] expected_remote_wake: Expected current state of the remote wakeup feature on the device
 */
void test_remote_wake_check(bool expected_remote_wake);

/**
 * @brief Finish the test
 *
 * Close the device, deregister the client..
 */
void test_remote_wake_finish(void);
