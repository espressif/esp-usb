/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @brief Test device setup
 */
void test_device_setup(void);

/**
 * @brief Test device release
 */
void test_device_release(void);

/**
 * @brief Test device wait
 *
 * Waits the tusb_mount_cb() which indicates the device connected to the Host and enumerated.
 */
void test_device_wait(void);
