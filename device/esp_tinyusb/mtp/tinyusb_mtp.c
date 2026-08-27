/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mtp/tinyusb_mtp_internal.h"
#include "mtp/tinyusb_mtp_context.h"
#include "mtp/tinyusb_mtp_object_store.h"
#include "mtp/tinyusb_mtp_transfer.h"

#if CONFIG_TINYUSB_MTP_ENABLED

static const char *TAG = "tinyusb_mtp";

static tinyusb_mtp_ctx_t s_mtp_context;
static SemaphoreHandle_t s_mtp_lifecycle_lock;
static portMUX_TYPE s_mtp_lifecycle_init_lock = portMUX_INITIALIZER_UNLOCKED;

tinyusb_mtp_ctx_t *mtp_context_get(void)
{
    return &s_mtp_context;
}

static SemaphoreHandle_t mtp_lifecycle_lock_get(void)
{
    portENTER_CRITICAL(&s_mtp_lifecycle_init_lock);
    SemaphoreHandle_t lock = s_mtp_lifecycle_lock;
    portEXIT_CRITICAL(&s_mtp_lifecycle_init_lock);
    return lock;
}

static esp_err_t mtp_lifecycle_init(void)
{
    if (mtp_lifecycle_lock_get() != NULL) {
        return ESP_OK;
    }

    SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(candidate != NULL, ESP_ERR_NO_MEM, TAG, "failed to create MTP lifecycle lock");
    portENTER_CRITICAL(&s_mtp_lifecycle_init_lock);
    if (s_mtp_lifecycle_lock == NULL) {
        s_mtp_lifecycle_lock = candidate;
        candidate = NULL;
    }
    portEXIT_CRITICAL(&s_mtp_lifecycle_init_lock);
    if (candidate != NULL) {
        vSemaphoreDelete(candidate);
    }
    return ESP_OK;
}

bool mtp_lifecycle_enter(void)
{
    SemaphoreHandle_t lock = mtp_lifecycle_lock_get();
    if (lock == NULL) {
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!s_mtp_context.installed) {
        xSemaphoreGive(lock);
        return false;
    }
    return true;
}

void mtp_lifecycle_exit(void)
{
    SemaphoreHandle_t lock = mtp_lifecycle_lock_get();
    if (lock == NULL) {
        ESP_LOGE(TAG, "MTP lifecycle lock is not initialized");
        abort();
    }
    xSemaphoreGive(lock);
}

static char *mtp_strdup_or_default(const char *value, const char *fallback)
{
    const char *src = (value && value[0]) ? value : fallback;
    char *copy = strdup(src);
    if (copy == NULL) {
        ESP_LOGE(TAG, "failed to allocate string copy");
    }
    return copy;
}

void mtp_lock(void)
{
    if (s_mtp_context.constant.lock == NULL) {
        ESP_LOGE(TAG, "MTP lock is not initialized");
        abort();
    }
    xSemaphoreTake(s_mtp_context.constant.lock, portMAX_DELAY);
}

void mtp_unlock(void)
{
    if (s_mtp_context.constant.lock == NULL) {
        ESP_LOGE(TAG, "MTP lock is not initialized");
        abort();
    }
    xSemaphoreGive(s_mtp_context.constant.lock);
}

bool mtp_session_is_open(void)
{
    mtp_lock();
    bool session_open = s_mtp_context.mux_protected.session_open;
    mtp_unlock();
    return session_open;
}

bool mtp_context_is_installed(void)
{
    return s_mtp_context.installed;
}

bool mtp_session_set_open_locked(bool open)
{
    bool previous = s_mtp_context.mux_protected.session_open;
    s_mtp_context.mux_protected.session_open = open;
    return previous;
}

static void mtp_free_driver_strings(void)
{
    free(s_mtp_context.constant.manufacturer);
    free(s_mtp_context.constant.model);
    free(s_mtp_context.constant.version);
    free(s_mtp_context.constant.serial);
    free(s_mtp_context.constant.friendly_name);
    s_mtp_context.constant.manufacturer = NULL;
    s_mtp_context.constant.model = NULL;
    s_mtp_context.constant.version = NULL;
    s_mtp_context.constant.serial = NULL;
    s_mtp_context.constant.friendly_name = NULL;
}

esp_err_t tinyusb_mtp_install_driver(const tinyusb_mtp_driver_config_t *config)
{
    esp_err_t ret = mtp_lifecycle_init();
    if (ret != ESP_OK) {
        return ret;
    }
    SemaphoreHandle_t lifecycle_lock = mtp_lifecycle_lock_get();
    xSemaphoreTake(lifecycle_lock, portMAX_DELAY);
    if (s_mtp_context.installed) {
        ESP_LOGW(TAG, "MTP driver already installed");
        xSemaphoreGive(lifecycle_lock);
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_mtp_context, 0, sizeof(s_mtp_context));
    s_mtp_context.constant.lock = xSemaphoreCreateMutex();
    if (s_mtp_context.constant.lock == NULL) {
        ESP_LOGE(TAG, "failed to create MTP lock");
        xSemaphoreGive(lifecycle_lock);
        return ESP_ERR_NO_MEM;
    }

    // Allocate large MTP tables on install so disabled MTP does not reserve static DRAM.
    s_mtp_context.mux_protected.storages = calloc(CONFIG_TINYUSB_MTP_MAX_STORAGES, sizeof(*s_mtp_context.mux_protected.storages));
    s_mtp_context.mux_protected.objects = calloc(CONFIG_TINYUSB_MTP_MAX_OBJECTS, sizeof(*s_mtp_context.mux_protected.objects));
    if (s_mtp_context.mux_protected.storages == NULL || s_mtp_context.mux_protected.objects == NULL) {
        ESP_LOGE(TAG, "failed to allocate MTP tables: storages=%d objects=%d", CONFIG_TINYUSB_MTP_MAX_STORAGES, CONFIG_TINYUSB_MTP_MAX_OBJECTS);
        mtp_free_object_table_locked();
        mtp_free_storage_table_locked();
        vSemaphoreDelete(s_mtp_context.constant.lock);
        memset(&s_mtp_context, 0, sizeof(s_mtp_context));
        xSemaphoreGive(lifecycle_lock);
        return ESP_ERR_NO_MEM;
    }

    s_mtp_context.constant.manufacturer = mtp_strdup_or_default(config ? config->manufacturer : NULL, MTP_DEFAULT_MANUFACTURER);
    s_mtp_context.constant.model = mtp_strdup_or_default(config ? config->model : NULL, MTP_DEFAULT_MODEL);
    s_mtp_context.constant.version = mtp_strdup_or_default(config ? config->version : NULL, MTP_DEFAULT_VERSION);
    s_mtp_context.constant.serial = mtp_strdup_or_default(config ? config->serial : NULL, MTP_DEFAULT_SERIAL);
    s_mtp_context.constant.friendly_name = mtp_strdup_or_default(config ? config->friendly_name : NULL, MTP_DEFAULT_FRIENDLY_NAME);
    if (!s_mtp_context.constant.manufacturer || !s_mtp_context.constant.model || !s_mtp_context.constant.version || !s_mtp_context.constant.serial || !s_mtp_context.constant.friendly_name) {
        mtp_free_driver_strings();
        mtp_free_object_table_locked();
        mtp_free_storage_table_locked();
        vSemaphoreDelete(s_mtp_context.constant.lock);
        memset(&s_mtp_context, 0, sizeof(s_mtp_context));
        xSemaphoreGive(lifecycle_lock);
        return ESP_ERR_NO_MEM;
    }

    s_mtp_context.mux_protected.next_object_handle = 1;
    s_mtp_context.mux_protected.next_scan_generation = 1;
    s_mtp_context.installed = true;
    ESP_LOGI(TAG, "MTP driver installed");
    xSemaphoreGive(lifecycle_lock);
    return ESP_OK;
}

esp_err_t tinyusb_mtp_uninstall_driver(void)
{
    if (!mtp_lifecycle_enter()) {
        ESP_LOGW(TAG, "MTP driver is not installed");
        return ESP_ERR_INVALID_STATE;
    }

    mtp_lock();
    s_mtp_context.installed = false;
    mtp_context_reset_transfers_locked(MTP_RESP_TRANSACTION_CANCELLED);
    mtp_free_object_table_locked();
    mtp_free_storage_table_locked();
    mtp_free_driver_strings();
    mtp_unlock();
    SemaphoreHandle_t lock = s_mtp_context.constant.lock;
    vSemaphoreDelete(lock);
    memset(&s_mtp_context, 0, sizeof(s_mtp_context));
    mtp_lifecycle_exit();
    return ESP_OK;
}

esp_err_t tinyusb_mtp_register_storage(const tinyusb_mtp_storage_config_t *config, tinyusb_mtp_storage_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(config && config->base_path && config->base_path[0] == '/', ESP_ERR_INVALID_ARG, TAG, "invalid MTP storage path");
    if (!mtp_lifecycle_enter()) {
        ESP_LOGW(TAG, "MTP driver is not installed");
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t total = 0;
    uint64_t free_bytes = 0;
    esp_err_t ret = esp_vfs_fat_info(config->base_path, &total, &free_bytes);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MTP storage path is not a mounted FATFS path: %s (%s)", config->base_path, esp_err_to_name(ret));
        mtp_lifecycle_exit();
        return ESP_ERR_NOT_FOUND;
    }

    mtp_lock();
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_STORAGES; i++) {
        if (s_mtp_context.mux_protected.storages[i].used && strcmp(s_mtp_context.mux_protected.storages[i].base_path, config->base_path) == 0) {
            mtp_unlock();
            mtp_lifecycle_exit();
            return ESP_ERR_INVALID_STATE;
        }
    }

    struct tinyusb_mtp_storage_s *storage = NULL;
    size_t index = 0;
    for (; index < CONFIG_TINYUSB_MTP_MAX_STORAGES; index++) {
        if (!s_mtp_context.mux_protected.storages[index].used) {
            storage = &s_mtp_context.mux_protected.storages[index];
            break;
        }
    }
    if (storage == NULL) {
        mtp_unlock();
        ESP_LOGE(TAG, "MTP storage table full, cannot register %s", config->base_path);
        mtp_lifecycle_exit();
        return ESP_FAIL;
    }

    storage->base_path = strdup(config->base_path);
    storage->display_name = mtp_strdup_or_default(config->display_name, config->base_path);
    if (storage->base_path == NULL || storage->display_name == NULL) {
        free(storage->base_path);
        free(storage->display_name);
        memset(storage, 0, sizeof(*storage));
        mtp_unlock();
        mtp_lifecycle_exit();
        return ESP_ERR_NO_MEM;
    }
    storage->removable = config->removable;
    storage->storage_id = MTP_STORAGE_ID(index);
    storage->used = true;
    if (handle) {
        *handle = storage;
    }
    ESP_LOGI(TAG, "MTP storage registered: id=0x%08" PRIx32 " path=%s name=%s total=%" PRIu64 " free=%" PRIu64,
             storage->storage_id, storage->base_path, storage->display_name, total, free_bytes);
    mtp_unlock();
    mtp_lifecycle_exit();
    return ESP_OK;
}

esp_err_t tinyusb_mtp_unregister_storage(tinyusb_mtp_storage_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "invalid MTP storage handle");
    if (!mtp_lifecycle_enter()) {
        ESP_LOGW(TAG, "MTP driver is not installed");
        return ESP_ERR_INVALID_STATE;
    }

    mtp_lock();
    if (!mtp_storage_handle_is_valid_locked(handle)) {
        ESP_LOGW(TAG, "invalid MTP storage handle");
        mtp_unlock();
        mtp_lifecycle_exit();
        return ESP_ERR_INVALID_ARG;
    }
    if (!mtp_transfer_is_idle_locked()) {
        ESP_LOGW(TAG, "MTP storage cannot be unregistered during an active transfer");
        mtp_unlock();
        mtp_lifecycle_exit();
        return ESP_ERR_INVALID_STATE;
    }
    mtp_transfer_detach_storage_locked(handle);
    mtp_clear_objects_for_storage_locked(handle);
    free(handle->base_path);
    free(handle->display_name);
    memset(handle, 0, sizeof(*handle));
    mtp_unlock();
    mtp_lifecycle_exit();
    return ESP_OK;
}

#endif
