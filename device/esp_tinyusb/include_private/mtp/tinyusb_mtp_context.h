/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "mtp/tinyusb_mtp_types.h"

tinyusb_mtp_ctx_t *mtp_context_get(void);
bool mtp_lifecycle_enter(void);
void mtp_lifecycle_exit(void);
void mtp_lock(void);
void mtp_unlock(void);
bool mtp_session_is_open(void);
bool mtp_context_is_installed(void);
bool mtp_session_set_open_locked(bool open);
void mtp_context_reset_transfers_locked(int32_t response);
