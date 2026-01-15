/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>

/**
 * @brief CDC-ACM mock device test modes
 */
enum cdc_acm_mock_dev_mode {
    MOCK_DEV_DUAL_IFACE = 0,            /**< CDC-ACM device with 2 interfaces */
    MOCK_DEV_EARLY_REMOTE_WAKE,         /**< CDC-ACM device with 2 interfaces, issuing early remote wakeup */
    MOCK_DEV_REMOTE_WAKE,               /**< CDC-ACM device with 2 interfaces, issuing remote wakeup signaling */
    MOCK_DEV_REMOTE_WAKE_DISCONNECT,    /**< CDC-ACM device with 2 interfaces, issuing remote wakeup signaling and sudden disconnect */
};

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Run CDC-ACM mock device in selected test mode
 *
 * @param[in] mode   Device test mode to set
 */
void cdc_acm_mock_device_run(const enum cdc_acm_mock_dev_mode mode);


#ifdef __cplusplus
}
#endif //__cplusplus
