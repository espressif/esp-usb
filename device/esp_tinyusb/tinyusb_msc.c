/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_vfs_fat.h"
#include "esp_partition.h"
#include "esp_memory_utils.h"
#include "soc/soc_caps.h"
#include "sdkconfig.h"
#include "vfs_fat_internal.h"
#include "tinyusb.h"
#include "device/usbd_pvt.h"
#include "class/msc/msc_device.h"

#include "storage_spiflash.h"
#include "msc_storage.h"
#include "tinyusb_msc.h"

#if (SOC_SDMMC_HOST_SUPPORTED)
#include "storage_sdmmc.h"
#include "diskio_sdmmc.h"
#endif // SOC_SDMMC_HOST_SUPPORTED

static const char *TAG = "tinyusb_msc_storage";

#define MSC_STORAGE_MEM_ALIGN 4
#define MSC_STORAGE_BUFFER_SIZE CONFIG_TINYUSB_MSC_BUFSIZE /*!< Size of the buffer, configured via menuconfig (MSC FIFO size) */

#if ((MSC_STORAGE_BUFFER_SIZE) % MSC_STORAGE_MEM_ALIGN != 0)
#error "CONFIG_TINYUSB_MSC_BUFSIZE must be divisible by MSC_STORAGE_MEM_ALIGN. Adjust your configuration (MSC FIFO size) in menuconfig."
#endif

#define TINYUSB_DEFAULT_BASE_PATH   CONFIG_TINYUSB_MSC_MOUNT_PATH /*!< Default base path for the filesystem, configured via menuconfig */

/**
 * @brief Structure representing a single write buffer for MSC operations.
 */
typedef struct {
    uint8_t data_buffer[MSC_STORAGE_BUFFER_SIZE]; /*!< Buffer to store write data. The size is defined by MSC_STORAGE_BUFFER_SIZE. */
    uint32_t lba;                          /*!< Logical Block Address for the current WRITE10 operation. */
    uint32_t offset;                       /*!< Offset within the specified LBA for the current write operation. */
    uint32_t bufsize;                      /*!< Number of bytes to be written in this operation. */
} msc_storage_buffer_t;

/**
 * @brief Handle for TinyUSB MSC storage interface.
 *
 * This structure holds metadata and function pointers required to
 * manage the underlying storage medium (SPI flash, SDMMC).
 */
typedef struct {
    // Storage related
    const storage_medium_t *medium;             /*!< Pointer to the storage medium. */
    tinyusb_msc_mount_point_t mount_point;      /*!< Current mount point type (application or USB host). */
    // Optimisation purpose
    uint32_t sector_count;                      /*!< Total number of sectors in the storage medium. */
    uint32_t sector_size;                       /*!< Size of a single sector in bytes. */
    // FS related
    struct {
        const char *base_path;                  /*!< Base path where the filesystem is mounted. */
        int max_files;                          /*!< Maximum number of files that can be open simultaneously. */
        bool do_not_format;                     /*!< If true, do not format the drive if filesystem is not present. */
        BYTE format_flags;                      /*!< Flags for formatting the filesystem, can be 0 to use default settings. */
    } fat_fs;
    // Buffer for storage operations
    msc_storage_buffer_t storage_buffer;        /*!< Buffer for storing data during write operations. */
} tinyusb_msc_storage_s;

typedef tinyusb_msc_storage_s msc_storage_obj_t;

typedef struct {
    struct {
        tinyusb_msc_storage_s *storage; /*!< Pointer to the storage handle. Supports only one storage. */
        uint8_t lun_count;              /*!< Number of logical units (LUNs) supported by the storage. */
    } dynamic;

    struct {
        union {
            struct {
                // User config - 16 bits
                uint32_t auto_mount_off: 1;         /**< If true, turn off automatically mount on USB host connection */
                uint32_t user_reserved15: 15;       /**< Reserved for future use */
                // Internal config - 16 bits
                uint32_t internal_reserved15: 15;   /**< Reserved for intenral use */
                uint32_t internally_installed: 1;   /**< Driver was internally installed. Uninstall driver on last storage removal */
            };
            uint32_t val;                           /**< MSC Driver configuration flag value */
        } flags;
        tusb_msc_callback_t event_cb;               /*!< Callback for mount changed events. */
        void *event_arg;                            /*!< Argument to pass to the event callback. */
    } constant;
} tinyusb_msc_driver_t;

static tinyusb_msc_driver_t *p_msc_driver;

// List of TODOs:
// 1.TODO: Multiple LUNs support
// 2.TODO: Critical section to protect the driver pointer
// 3.TODO: Critical section to protect the callback
// 4.TODO: Storage Format support

//
// ========================== TinyUSB MSC Storage Event Handling =================================
//

static inline void tinyusb_event_cb(tinyusb_msc_event_id_t event_id)
{
    assert(p_msc_driver != NULL);
    tinyusb_msc_event_t event = {
        .id = event_id,
        .mount_point = p_msc_driver->dynamic.storage->mount_point,
    };
    p_msc_driver->constant.event_cb((tinyusb_msc_storage_handle_t)p_msc_driver->dynamic.storage,
                                    &event,
                                    p_msc_driver->constant.event_arg);
}

//
// ========================== TinyUSB MSC Storage Operations =================================
//

static inline esp_err_t msc_storage_read_sector(uint8_t lun, uint32_t lba, uint32_t offset, size_t size, void *dest)
{
    (void) lun; // Not used, as we support only one LUN
    assert(p_msc_driver); // Sanity check
    assert(p_msc_driver->dynamic.storage); // Ensure storage is initialized
    assert(p_msc_driver->dynamic.storage->medium); // Ensure medium is set

    const storage_medium_t *medium = p_msc_driver->dynamic.storage->medium;
    return medium->read(lba, offset, size, dest);
}

static inline esp_err_t msc_storage_write_sector(uint8_t lun, uint32_t lba, uint32_t offset, size_t size, const void *src)
{
    (void) lun; // Not used, as we support only one LUN
    assert(p_msc_driver); // Sanity check
    assert(p_msc_driver->dynamic.storage);          // Ensure storage is initialized
    assert(p_msc_driver->dynamic.storage->medium);  // Ensure medium is set

    const storage_medium_t *medium = p_msc_driver->dynamic.storage->medium;
    return medium->write(lba, offset, size, src);
}

/**
 * @brief Handles deferred USB MSC write operations.
 *
 * This function is invoked via TinyUSB's deferred execution mechanism to perform
 * write operations to the underlying storage. It writes data from the
 * `storage_buffer` stored within the `s_storage_handle`.
 *
 * @param param Unused. Present for compatibility with deferred function signature.
 */
static void tusb_write_func(void *param)
{
    assert(param); // Ensure param is not NULL
    // Process the data in storage_buffer
    msc_storage_obj_t *s_storage_handle = p_msc_driver->dynamic.storage;
    esp_err_t err = msc_storage_write_sector(
                        0, // LUN is not used, as we support only one LUN
                        s_storage_handle->storage_buffer.lba,
                        s_storage_handle->storage_buffer.offset,
                        s_storage_handle->storage_buffer.bufsize,
                        (const void *)s_storage_handle->storage_buffer.data_buffer
                    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Write failed, error=0x%x", err);
    }
}

static inline esp_err_t msc_storage_write_sector_deferred(uint8_t lun, uint32_t lba, uint32_t offset, size_t size, const void *src)
{
    (void) lun; // Not used, as we support only one LUN
    assert(p_msc_driver); // Sanity check
    assert(p_msc_driver->dynamic.storage);          // Ensure storage is initialized
    assert(p_msc_driver->dynamic.storage->medium);  // Ensure medium is set

    msc_storage_obj_t *s_storage_handle = p_msc_driver->dynamic.storage;

    // As we defer the write operation to the TinyUSB task, we need to ensure that
    // the address does not overflow for SPI Flash storage medium
    if (s_storage_handle->medium->type == STORAGE_MEDIUM_TYPE_SPIFLASH) {
        size_t addr = 0; // Address of the data to be read, relative to the beginning of the partition.
        size_t temp = 0;
        size_t sector_size = p_msc_driver->dynamic.storage->sector_size;
        ESP_RETURN_ON_FALSE(!__builtin_umul_overflow(lba, sector_size, &temp), ESP_ERR_INVALID_SIZE, TAG, "overflow lba %lu sector_size %u", lba, sector_size);
        ESP_RETURN_ON_FALSE(!__builtin_uadd_overflow(temp, offset, &addr), ESP_ERR_INVALID_SIZE, TAG, "overflow addr %u offset %lu", temp, offset);
    }

    // Copy data to the buffer
    memcpy((void *)s_storage_handle->storage_buffer.data_buffer, src, size);
    s_storage_handle->storage_buffer.lba = lba;
    s_storage_handle->storage_buffer.offset = offset;
    s_storage_handle->storage_buffer.bufsize = size;

    // Defer execution of the write to the TinyUSB task
    usbd_defer_func(tusb_write_func, (void *)s_storage_handle, false);

    return ESP_OK;
}

static esp_err_t vfs_fat_format(BYTE format_flags)
{
    esp_err_t ret;
    FRESULT fresult;
    // Drive does not have a filesystem, try to format it
    const size_t workbuf_size = 4096;
    void *workbuf = ff_memalloc(workbuf_size);
    if (workbuf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t alloc_unit_size = esp_vfs_fat_get_allocation_unit_size(CONFIG_WL_SECTOR_SIZE, workbuf_size);

    ESP_LOGW(TAG, "Format drive, allocation unit size=%d", alloc_unit_size);

    const MKFS_PARM opt = {format_flags, 0, 0, 0, alloc_unit_size};
    fresult = f_mkfs("", &opt, workbuf, workbuf_size); // Use default volume
    if (fresult != FR_OK) {
        ret = ESP_FAIL;
        ESP_LOGE(TAG, "Unable to create default volume, (%d)", fresult);
        goto fail;
    }
    ff_memfree(workbuf);
    workbuf = NULL;
    return ESP_OK;
fail:
    if (workbuf) {
        ff_memfree(workbuf);
    }
    return ret;
}

static esp_err_t vfs_fat_mount(char *drv, FATFS *fs, bool force)
{
    esp_err_t ret;
    // Try to mount the drive
    FRESULT fresult = f_mount(fs, drv, force ? 1 : 0);
    switch (fresult) {
    case FR_OK:
        ESP_LOGD(TAG, "Mounted drive %s successfully", drv);
        ret = ESP_OK;
        break;
    case FR_NO_FILESYSTEM:
    case FR_INT_ERR:
        ESP_LOGD(TAG, "Drive %s does not have a filesystem, need to format", drv);
        ret = ESP_ERR_NOT_FOUND; // No filesystem or internal error, need to format
        break;
    default:
        ESP_LOGE(TAG, "Failed to mount drive %s (%d)", drv, fresult);
        ret = ESP_FAIL; // Other errors
        break;
    }
    return ret;
}

esp_err_t msc_storage_mount(void)
{
    esp_err_t ret;
    assert(p_msc_driver != NULL);
    tinyusb_msc_storage_s *s_storage_handle = p_msc_driver->dynamic.storage;

    FATFS *fs = NULL;
    const char *base_path = s_storage_handle->fat_fs.base_path;
    int max_files = s_storage_handle->fat_fs.max_files;

    if (s_storage_handle->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
        // If the storage is already mounted to APP, no need to unmount
        return ESP_OK;
    }

    tinyusb_event_cb(TINYUSB_MSC_EVENT_MOUNT_START);

    // Mount VFS FAT at base path
    BYTE pdrv = 0xFF;
    ESP_RETURN_ON_ERROR(ff_diskio_get_drive(&pdrv), TAG, "The maximum count of volumes is already mounted");

    ESP_GOTO_ON_ERROR(s_storage_handle->medium->mount(pdrv), fail, TAG, "Failed pdrv=%d", pdrv);

    char drv[3] = {(char)('0' + pdrv), ':', 0};
    ret = esp_vfs_fat_register(base_path, drv, max_files, &fs);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "VFS FAT already registered");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "VFS FAT register failed, %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = vfs_fat_mount(drv, fs, true);
    if (ret == ESP_ERR_NOT_FOUND) {
        // If mount failed, try to format the drive
        if (s_storage_handle->fat_fs.do_not_format) {
            ESP_LOGE(TAG, "Mount failed and do not format is set");
            tinyusb_event_cb(TINYUSB_MSC_EVENT_FORMAT_REQUIRED);
            goto fail;
        }
        ESP_LOGW(TAG, "Mount failed, trying to format the drive");
        BYTE format_flags = s_storage_handle->fat_fs.format_flags;
        ret = vfs_fat_format(format_flags);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to format the drive, %s", esp_err_to_name(ret));
            goto fail;
        }
        ESP_GOTO_ON_ERROR(vfs_fat_mount(drv, fs, false), fail, TAG, "Failed to mount FAT filesystem");
        ESP_LOGW(TAG, "Format completed, FAT mounted successfully");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount drive, %s", esp_err_to_name(ret));
        goto fail; // If mount was unsuccessful, return the error
    }

    s_storage_handle->mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP;

    tinyusb_event_cb(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);

    return ret;

fail:
    if (fs) {
        esp_vfs_fat_unregister_path(base_path);
    }
    ff_diskio_unregister(pdrv);
    return ret;
}

static esp_err_t msc_storage_unmount(void)
{
    tinyusb_msc_storage_s *s_storage_handle = p_msc_driver->dynamic.storage;

    if (!s_storage_handle) {
        return ESP_FAIL;
    }

    if (s_storage_handle->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB) {
        // If the storage is already mounted to USB, no need to unmount
        return ESP_OK;
    }

    tinyusb_event_cb(TINYUSB_MSC_EVENT_MOUNT_START);

    esp_err_t err = s_storage_handle->medium->unmount();
    if (err) {
        return err;
    }
    err = esp_vfs_fat_unregister_path(s_storage_handle->fat_fs.base_path);
    s_storage_handle->mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;

    tinyusb_event_cb(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);

    return err;
}

static void msc_storage_event_default_cb(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    (void) handle;
    (void) event;
    (void) arg;

    // Default callback does nothing
    // This is used when no user-defined callback is provided
    ESP_LOGW(TAG, "Default MSC event callback called, event ID: %d, mount point: %d", event->id, event->mount_point);
}

static esp_err_t msc_storage_config(msc_storage_obj_t *storage_obj,
                                    const tinyusb_msc_storage_config_t *config,
                                    const storage_medium_t *medium)
{
    storage_obj->medium = medium;
    storage_obj->mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB; // Default mount point is USB host
    // In case the user does not set mount_config.max_files
    // and for backward compatibility with versions <1.4.2
    // max_files is set to 2
    const int max_files = config->fat_fs.config.max_files;
    storage_obj->fat_fs.max_files = max_files > 0 ? max_files : 2;
    storage_obj->fat_fs.do_not_format = config->fat_fs.do_not_format;
    storage_obj->fat_fs.format_flags = config->fat_fs.format_flags;
    if (storage_obj->fat_fs.format_flags == 0) {
        // Use default format flags if not provided
        storage_obj->fat_fs.format_flags = FM_ANY; // Auto-select FAT type based on volume size
    }
    if (config->fat_fs.base_path == NULL) {
        // Use default base path if not provided
        storage_obj->fat_fs.base_path = TINYUSB_DEFAULT_BASE_PATH;
    } else {
        // Use the provided base path
        storage_obj->fat_fs.base_path = config->fat_fs.base_path;
    }

    // Set sector count and size
    storage_info_t storage_info;
    ESP_RETURN_ON_ERROR(storage_obj->medium->get_info(&storage_info), TAG, "Failed to get storage info");
    storage_obj->sector_count = storage_info.total_sectors;
    storage_obj->sector_size = storage_info.sector_size;

    ESP_LOGD(TAG, "Storage type: , sectors count: %"PRIu32", sector size: %"PRIu32"",
             storage_obj->sector_count,
             storage_obj->sector_size);

    return ESP_OK;
}

//
// ============================ TinyUSB MSC Storage Private Functions ==========================
//

void msc_storage_mount_to_app(void)
{
    if (p_msc_driver == NULL ||
            p_msc_driver->dynamic.storage == NULL ||
            p_msc_driver->constant.flags.auto_mount_off) {
        return;
    }

    if (msc_storage_mount() != ESP_OK) {
        ESP_LOGW(TAG, "Unable to mount storage to app");
        tinyusb_event_cb(TINYUSB_MSC_EVENT_MOUNT_FAILED);
    }
}

void msc_storage_mount_to_usb(void)
{
    if (p_msc_driver == NULL ||
            p_msc_driver->dynamic.storage == NULL ||
            p_msc_driver->constant.flags.auto_mount_off) {
        return;
    }

    if (msc_storage_unmount() != ESP_OK) {
        ESP_LOGW(TAG, "Unable to mount storage to usb");
        tinyusb_event_cb(TINYUSB_MSC_EVENT_MOUNT_FAILED);
    }
}

//
// ========================== TinyUSB MSC Public API Functions =================================
//

esp_err_t tinyusb_msc_set_storage_callback(tusb_msc_callback_t callback, void *arg)
{
    ESP_RETURN_ON_FALSE(p_msc_driver != NULL, ESP_ERR_INVALID_STATE, TAG, "Storage handle is not initialized");
    ESP_RETURN_ON_FALSE(callback != NULL, ESP_ERR_INVALID_ARG, TAG, "Callback can't be NULL");

    // TODO: add critical section to protect the callback
    p_msc_driver->constant.event_cb = callback;
    p_msc_driver->constant.event_arg = arg;

    return ESP_OK;
}

esp_err_t tinyusb_msc_install_driver(const tinyusb_msc_driver_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Config can't be NULL");
    ESP_RETURN_ON_FALSE(p_msc_driver == NULL, ESP_ERR_INVALID_STATE, TAG, "MSC driver is already initialized");

    tinyusb_msc_driver_t *msc_driver = NULL;
    msc_driver = (tinyusb_msc_driver_t *)heap_caps_aligned_calloc(MSC_STORAGE_MEM_ALIGN, sizeof(tinyusb_msc_driver_t), sizeof(uint32_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(msc_driver != NULL, ESP_ERR_NO_MEM, TAG, "Failed to allocate memory for MSC driver");

    // Default callback
    if (config->callback == NULL) {
        msc_driver->constant.event_cb = msc_storage_event_default_cb;
        msc_driver->constant.event_arg = NULL;
    } else {
        msc_driver->constant.event_cb = config->callback;
        msc_driver->constant.event_arg = config->callback_arg;
    }
    // TODO: Multiple LUNs support
    msc_driver->dynamic.lun_count = 0; // LUN will be added with storage initialization
    msc_driver->constant.flags.val = (uint16_t) config->user_flags.val; // Config flags for the MSC driver

    // TODO: Critical section to protect the driver pointer
    p_msc_driver = msc_driver;
    return ESP_OK;
}

esp_err_t tinyusb_msc_uninstall_driver(void)
{
    ESP_RETURN_ON_FALSE(p_msc_driver != NULL, ESP_ERR_NOT_FOUND, TAG, "MSC driver is not initialized");
    ESP_RETURN_ON_FALSE(p_msc_driver->dynamic.storage == NULL, ESP_ERR_INVALID_STATE, TAG, "MSC storage is initialized");
    ESP_RETURN_ON_FALSE(p_msc_driver->dynamic.lun_count == 0, ESP_ERR_INVALID_STATE, TAG, "LUN count is not zero, can't uninstall MSC driver");
    tinyusb_msc_driver_t *msc_driver = p_msc_driver;

    // TODO: Critical section to protect the driver pointer
    p_msc_driver = NULL;

    // Free the driver memory
    heap_caps_free(msc_driver);
    return ESP_OK;
}

esp_err_t tinyusb_msc_new_storage_spiflash(const tinyusb_msc_storage_config_t *config,
        tinyusb_msc_storage_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Config can't be NULL");
    ESP_RETURN_ON_FALSE(config->medium.wl_handle != WL_INVALID_HANDLE, ESP_ERR_INVALID_ARG, TAG, "Wear levelling handle should be valid");

    ESP_RETURN_ON_FALSE(CONFIG_TINYUSB_MSC_BUFSIZE >= CONFIG_WL_SECTOR_SIZE,
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "TinyUSB buffer size (%d) must be at least the size of Wear Levelling sector size (%d), please reconfigure the project.",
                        (int)(CONFIG_TINYUSB_MSC_BUFSIZE), (int)(CONFIG_WL_SECTOR_SIZE));

    if (p_msc_driver == NULL) {
        // Driver was not installed, install it now
        tinyusb_msc_driver_config_t default_cfg = {
            .user_flags = {
                .auto_mount_off = false, // Disable auto-mount
            },
            .callback = msc_storage_event_default_cb, // Default callback
            .callback_arg = NULL, // No argument for the default callback
        };
        ESP_RETURN_ON_ERROR(tinyusb_msc_install_driver(&default_cfg), TAG, "Failed to install MSC driver");
        p_msc_driver->constant.flags.internally_installed = true; // Mark that the driver was installed internally
    }

    msc_storage_obj_t *storage_obj = (msc_storage_obj_t *)heap_caps_aligned_calloc(MSC_STORAGE_MEM_ALIGN, sizeof(msc_storage_obj_t), sizeof(uint32_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(storage_obj != NULL, ESP_ERR_NO_MEM, TAG, "Failed to allocate memory for MSC storage");

    // Create a storage based on the config
    const storage_medium_t *medium = NULL;
    ESP_RETURN_ON_ERROR(storage_spiflash_open_medium(config->medium.wl_handle, &medium), TAG, "Failed to open SPI Flash medium");
    ESP_RETURN_ON_ERROR(msc_storage_config(storage_obj, config, medium), TAG, "Failed to configure MSC storage");

    // Set the storage handle in the driver
    p_msc_driver->dynamic.storage = storage_obj;
    p_msc_driver->dynamic.lun_count++;

    // Mount the storage if it is configured to be mounted to application
    if (config->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
        ESP_RETURN_ON_ERROR(msc_storage_mount(), TAG, "Failed to mount storage to application");
    }

    // Return the handle to the storage
    if (handle != NULL) {
        *handle = (tinyusb_msc_storage_handle_t)storage_obj;
    }
    return ESP_OK;
}

#if (SOC_SDMMC_HOST_SUPPORTED)
esp_err_t tinyusb_msc_new_storage_sdmmc(const tinyusb_msc_storage_config_t *config,
                                        tinyusb_msc_storage_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Config can't be NULL");
    ESP_RETURN_ON_FALSE(config->medium.card != NULL, ESP_ERR_INVALID_ARG, TAG, "Card handle should be valid");

    if (p_msc_driver == NULL) {
        // Driver was not installed, install it now
        tinyusb_msc_driver_config_t default_cfg = {
            .user_flags = {
                .auto_mount_off = false, // Disable auto-mount
            },
            .callback = msc_storage_event_default_cb, // Default callback
            .callback_arg = NULL, // No argument for the default callback
        };
        ESP_RETURN_ON_ERROR(tinyusb_msc_install_driver(&default_cfg), TAG, "Failed to install MSC driver");
        p_msc_driver->constant.flags.internally_installed = true; // Mark that the driver was installed internally
    }

    msc_storage_obj_t *storage_obj = (tinyusb_msc_storage_s *)heap_caps_aligned_calloc(MSC_STORAGE_MEM_ALIGN, sizeof(tinyusb_msc_storage_s), sizeof(uint32_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(storage_obj != NULL, ESP_ERR_NO_MEM, TAG, "Failed to allocate memory for MSC storage");
    // Create a storage
    const storage_medium_t *medium = NULL;
    ESP_RETURN_ON_ERROR(storage_sdmmc_open_medium(config->medium.card, &medium), TAG, "Failed to open SD/MMC medium");
    ESP_RETURN_ON_ERROR(msc_storage_config(storage_obj, config, medium), TAG, "Failed to configure MSC storage");
    // Set the storage handle in the driver
    p_msc_driver->dynamic.storage = storage_obj;
    p_msc_driver->dynamic.lun_count++;
    // Mount the storage if it is configured to be mounted to application
    if (config->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
        // Mount to application
        ESP_RETURN_ON_ERROR(msc_storage_mount(), TAG, "Failed to mount storage to application");
    }
    // Return the handle to the storage
    if (handle != NULL) {
        *handle = (tinyusb_msc_storage_handle_t)storage_obj;
    }
    return ESP_OK;
}
#endif // SOC_SDMMC_HOST_SUPPORTED

esp_err_t tinyusb_msc_delete_storage(tinyusb_msc_storage_handle_t handle)
{
    (void) handle; // Unused parameter, we use the global storage handle
    ESP_RETURN_ON_FALSE(p_msc_driver != NULL, ESP_ERR_INVALID_STATE, TAG, "MSC driver is not initialized");

    // TODO: Multiple LUNs support
    assert(p_msc_driver->dynamic.storage);
    assert(p_msc_driver->dynamic.storage->medium);

    if (p_msc_driver->dynamic.storage->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
        // Unmount the storage if it is mounted to application
        ESP_ERROR_CHECK(msc_storage_unmount());
    }
    p_msc_driver->dynamic.storage->medium->close();
    heap_caps_free(p_msc_driver->dynamic.storage);

    p_msc_driver->dynamic.storage = NULL; // Clear the storage pointer
    assert(p_msc_driver->dynamic.lun_count > 0); // Sanity check to ensure LUN count is positive
    p_msc_driver->dynamic.lun_count--; // Decrease the LUN count

    if (p_msc_driver->dynamic.lun_count == 0 && p_msc_driver->constant.flags.internally_installed) {
        // If no LUNs left and driver was installed internally, uninstall the driver
        ESP_ERROR_CHECK(tinyusb_msc_uninstall_driver()); // Should never fail
    }
    return ESP_OK;
}

esp_err_t tinyusb_msc_get_storage_capacity(tinyusb_msc_storage_handle_t handle, uint32_t *sector_count)
{
    (void) handle; // Unused parameter, we use the global s_storage_handle
    ESP_RETURN_ON_FALSE(p_msc_driver != NULL, ESP_ERR_INVALID_STATE, TAG, "MSC driver is not initialized");
    ESP_RETURN_ON_FALSE(p_msc_driver->dynamic.storage != NULL, ESP_ERR_INVALID_STATE, TAG, "MSC storage is not initialized");
    ESP_RETURN_ON_FALSE(sector_count != NULL, ESP_ERR_INVALID_ARG, TAG, "Sector count pointer can't be NULL");

    *sector_count = p_msc_driver->dynamic.storage->sector_count;

    return ESP_OK;
}

esp_err_t tinyusb_msc_get_storage_sector_size(tinyusb_msc_storage_handle_t handle, uint32_t *sector_size)
{
    (void) handle; // Unused parameter, we use the global s_storage_handle
    ESP_RETURN_ON_FALSE(p_msc_driver != NULL, ESP_ERR_INVALID_STATE, TAG, "MSC driver is not initialized");
    ESP_RETURN_ON_FALSE(p_msc_driver->dynamic.storage != NULL, ESP_ERR_INVALID_STATE, TAG, "MSC storage is not initialized");
    ESP_RETURN_ON_FALSE(sector_size != NULL, ESP_ERR_INVALID_ARG, TAG, "Sector size pointer can't be NULL");

    *sector_size = p_msc_driver->dynamic.storage->sector_size;

    return ESP_OK;
}

esp_err_t tinyusb_msc_set_storage_mount_point(tinyusb_msc_storage_handle_t handle,
        tinyusb_msc_mount_point_t mount_point)
{
    (void) handle; // Unused parameter, we use the global storage handle

    ESP_RETURN_ON_FALSE(p_msc_driver != NULL, ESP_ERR_INVALID_STATE, TAG, "MSC driver is not initialized");
    ESP_RETURN_ON_FALSE(p_msc_driver->dynamic.storage != NULL, ESP_ERR_INVALID_STATE, TAG, "MSC storage is not initialized");

    if (mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
        // If the storage is mounted to application, mount it
        msc_storage_mount();
    } else {
        // If the storage is mounted to USB host, unmount it
        msc_storage_unmount();
    }
    p_msc_driver->dynamic.storage->mount_point = mount_point;

    return ESP_OK;
}

esp_err_t tinyusb_msc_config_storage_fat_fs(tinyusb_msc_storage_handle_t handle,
        tinyusb_msc_fatfs_config_t *fatfs_config)
{
    (void) handle; // Unused parameter, we use the global s_storage_handle
    ESP_RETURN_ON_FALSE(p_msc_driver != NULL, ESP_ERR_INVALID_STATE, TAG, "MSC driver is not initialized");
    ESP_RETURN_ON_FALSE(p_msc_driver->dynamic.storage != NULL, ESP_ERR_INVALID_STATE, TAG, "MSC storage is not initialized");
    ESP_RETURN_ON_FALSE(fatfs_config != NULL, ESP_ERR_INVALID_ARG, TAG, "FatFS config pointer can't be NULL");

    // 2.TODO: Critical section to protect the storage object
    tinyusb_msc_storage_s *storage_obj = p_msc_driver->dynamic.storage;
    // In case the user does not set mount_config.max_files
    // and for backward compatibility with versions <1.4.2
    // max_files is set to 2
    const int max_files = fatfs_config->config.max_files;
    storage_obj->fat_fs.max_files = max_files > 0 ? max_files : 2;
    storage_obj->fat_fs.do_not_format = fatfs_config->do_not_format;
    storage_obj->fat_fs.format_flags = fatfs_config->format_flags;
    if (storage_obj->fat_fs.format_flags == 0) {
        // Use default format flags if not provided
        storage_obj->fat_fs.format_flags = FM_ANY; // Auto-select FAT type based on volume size
    }
    if (fatfs_config->base_path == NULL) {
        // Use default base path if not provided
        storage_obj->fat_fs.base_path = TINYUSB_DEFAULT_BASE_PATH;
    } else {
        // Use the provided base path
        storage_obj->fat_fs.base_path = fatfs_config->base_path;
    }

    return ESP_OK;
}

esp_err_t tinyusb_msc_get_storage_mount_point(tinyusb_msc_storage_handle_t handle,
        tinyusb_msc_mount_point_t *mount_point)
{
    (void) handle; // Unused parameter, we use the global s_storage_handle
    ESP_RETURN_ON_FALSE(p_msc_driver != NULL, ESP_ERR_INVALID_STATE, TAG, "MSC driver is not initialized");
    ESP_RETURN_ON_FALSE(p_msc_driver->dynamic.storage != NULL, ESP_ERR_INVALID_STATE, TAG, "MSC storage is not initialized");
    ESP_RETURN_ON_FALSE(mount_point != NULL, ESP_ERR_INVALID_ARG, TAG, "Mount point pointer can't be NULL");

    *mount_point = p_msc_driver->dynamic.storage->mount_point;

    return ESP_OK;
}

esp_err_t tinyusb_msc_format_storage(tinyusb_msc_storage_handle_t handle)
{
    (void) handle; // Unused parameter
    // TODO: Storage Format support
    ESP_LOGW(TAG, "Formatting drive is not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}

/* TinyUSB MSC callbacks
   ********************************************************************* */

/** SCSI ASC/ASCQ codes. **/
/** User can add and use more codes as per the need of the application **/
#define SCSI_CODE_ASC_MEDIUM_NOT_PRESENT                0x3A /** SCSI ASC code for 'MEDIUM NOT PRESENT' **/
#define SCSI_CODE_ASC_INVALID_COMMAND_OPERATION_CODE    0x20 /** SCSI ASC code for 'INVALID COMMAND OPERATION CODE' **/
#define SCSI_CODE_ASCQ                                  0x00

// Invoked when received GET_MAX_LUN request, required for multiple LUNs implementation
uint8_t tud_msc_get_maxlun_cb(void)
{
    uint8_t msc_lun = 1; // Default 1 LUN, even when the storage is not initialized, report 1 LUN, but without a media

    if (p_msc_driver != NULL && p_msc_driver->dynamic.lun_count) {
        msc_lun = p_msc_driver->dynamic.lun_count;
    }

    return msc_lun;
}

// Invoked when received SCSI_CMD_INQUIRY
// Application fill vendor id, product id and revision with string up to 8, 16, 4 characters respectively
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void) lun;
    const char vid[] = "TinyUSB";
    const char pid[] = "TEST MSC Storage";
    const char rev[] = "0.1";

    memcpy(vendor_id, vid, strlen(vid));
    memcpy(product_id, pid, strlen(pid));
    memcpy(product_rev, rev, strlen(rev));
}

// Invoked when received Test Unit Ready command.
// return true allowing host to read/write this LUN e.g SD card inserted
bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void) lun;
    bool unit_ready;

    if (p_msc_driver != NULL && p_msc_driver->dynamic.storage != NULL &&
            p_msc_driver->dynamic.storage->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB) {
        // Storage media is ready for access by USB host
        unit_ready = true;
    } else {
        // Storage media is not ready for access by USB host
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, SCSI_CODE_ASC_MEDIUM_NOT_PRESENT, SCSI_CODE_ASCQ);
        unit_ready = false;
    }

    return unit_ready;
}

// Invoked when received SCSI_CMD_READ_CAPACITY_10 and SCSI_CMD_READ_FORMAT_CAPACITY to determine the disk size
// Application update block count and block size
void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void) lun;

    uint32_t capacity = 0;
    uint32_t sec_size = 0;

    // Do not verify the error, if the storage is not initialized, it will return 0 for both sector count and size
    tinyusb_msc_get_storage_capacity(NULL, &capacity);
    tinyusb_msc_get_storage_sector_size(NULL, &sec_size);

    *block_count = capacity;
    *block_size  = (uint16_t)sec_size;
}

// Invoked when received Start Stop Unit command
// - Start = 0 : stopped power mode, if load_eject = 1 : unload disk storage
// - Start = 1 : active mode, if load_eject = 1 : load disk storage
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void) lun;
    (void) power_condition;

    if (load_eject && !start) {
        // Eject media from the storage
        msc_storage_mount_to_app();
    }
    return true;
}

// Invoked when received SCSI READ10 command
// - Address = lba * BLOCK_SIZE + offset
// - Application fill the buffer (up to bufsize) with address contents and return number of read byte.
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    esp_err_t err = msc_storage_read_sector(lun, lba, offset, bufsize, buffer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "READ(10) command failed, %s", esp_err_to_name(err));
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_CODE_ASC_INVALID_COMMAND_OPERATION_CODE, SCSI_CODE_ASCQ);
        return -1; // Indicate an error occurred
    }
    return bufsize;
}

// Invoked when received SCSI WRITE10 command
// - Address = lba * BLOCK_SIZE + offset
// - Application write data from buffer to address contents (up to bufsize) and return number of written byte.
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void) lun; // LUN is not used, as we support only one LUN
    // There is no way to return the error from the deferred function, so we need to check everything here
    if (bufsize > MSC_STORAGE_BUFFER_SIZE) {
        ESP_LOGE(TAG, "Buffer size %"PRIu32" exceeds maximum allowed size %d", bufsize, MSC_STORAGE_BUFFER_SIZE);
        goto error;
    }
    esp_err_t err = msc_storage_write_sector_deferred(lun, lba, offset, bufsize, buffer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WRITE(10) command failed, %s", esp_err_to_name(err));
        goto error;
    }
    // Return the number of bytes accepted
    return bufsize;

error:
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_CODE_ASC_INVALID_COMMAND_OPERATION_CODE, SCSI_CODE_ASCQ);
    return -1; // Indicate an error occurred
}

/**
 * Invoked when received an SCSI command not in built-in list below.
 * - READ_CAPACITY10, READ_FORMAT_CAPACITY, INQUIRY, TEST_UNIT_READY, START_STOP_UNIT, MODE_SENSE6, REQUEST_SENSE
 * - READ10 and WRITE10 has their own callbacks
 *
 * \param[in]   lun         Logical unit number
 * \param[in]   scsi_cmd    SCSI command contents which application must examine to response accordingly
 * \param[out]  buffer      Buffer for SCSI Data Stage.
 *                            - For INPUT: application must fill this with response.
 *                            - For OUTPUT it holds the Data from host
 * \param[in]   bufsize     Buffer's length.
 *
 * \return      Actual bytes processed, can be zero for no-data command.
 * \retval      negative    Indicate error e.g unsupported command, tinyusb will \b STALL the corresponding
 *                          endpoint and return failed status in command status wrapper phase.
 */
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    int32_t ret;

    switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        /* SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL is the Prevent/Allow Medium Removal
        command (1Eh) that requests the library to enable or disable user access to
        the storage media/partition. */
        ret = 0;
        break;
    default:
        ESP_LOGW(TAG, "tud_msc_scsi_cb() invoked: %d", scsi_cmd[0]);
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_CODE_ASC_INVALID_COMMAND_OPERATION_CODE, SCSI_CODE_ASCQ);
        ret = -1;
        break;
    }
    return ret;
}

/*********************************************************************** TinyUSB MSC callbacks*/
