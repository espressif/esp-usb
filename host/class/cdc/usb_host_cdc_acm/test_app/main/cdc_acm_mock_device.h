/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>

/**
 * @brief CDC-ACM mock device test modes
 */
enum test_cdc_acm_mock_device {
    TEST_CDC_ACM_MOCK_DEVICE_WITH_TWO_IFACES = 0,
    TEST_CDC_ACM_MOCK_DEVICE_REMOTE_WAKE_BASIC,
    TEST_CDC_ACM_MOCK_DEVICE_REMOTE_WAKE_SUDDEN_DISCONNECT,
    TEST_CDC_ACM_MOCK_DEVICE_MAX,
};

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief RUn CDC-ACM mock device in selected mode
 */
void cdc_acm_mock_device_run(void);

/**
 * @brief Set CDC-ACM mock device test mode
 *
 * Note: Should be called before cdc_acm_mock_device_run()
 *
 * @param[in] mode   Device test mode to set
 */
void cdc_acm_mock_device_set_mode(const enum test_cdc_acm_mock_device mode);


#ifdef __cplusplus
}
#endif //__cplusplus
