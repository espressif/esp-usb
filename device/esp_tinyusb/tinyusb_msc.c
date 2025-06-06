/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
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
    void *context;                              /*!< Context for the storage medium, can be a wear leveling handle or SDMMC card handle. */
    const storage_api_t *storage;               /*!< Pointer to the storage API for the medium. */
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
    // Callbacks
    tusb_msc_callback_t event_cb;               /*!< Callback for mount changed events. */
    void *event_arg;                            /*!< Argument to pass to the event callback. */
} tinyusb_msc_storage_handle_s;

/* handle of tinyusb driver connected to application */
static tinyusb_msc_storage_handle_s *s_storage_handle;

static inline void tinyusb_event_cb(tinyusb_msc_event_id_t event_id)
{
    assert(s_storage_handle);
    tinyusb_msc_event_t event = {
        .id = event_id,
        .mount_point = s_storage_handle->mount_point,
    };
    s_storage_handle->event_cb(&event);
}

// Storage operations
static inline esp_err_t msc_storage_read_sector(uint32_t lba, uint32_t offset, size_t size, void *dest)
{
    assert(s_storage_handle);
    void *context = s_storage_handle->context;
    return s_storage_handle->storage->read(context, lba, offset, size, dest);
}

static inline esp_err_t msc_storage_write_sector(uint32_t lba, uint32_t offset, size_t size, const void *src)
{
    assert(s_storage_handle);
    void *context = s_storage_handle->context;
    return s_storage_handle->storage->write(context, lba, offset, size, src);
}

static esp_err_t vfs_fat_format(BYTE format_flags)
{
    esp_err_t ret;
    FRESULT fresult;
    // Drive does not have a filesystem, try to format it
    const size_t workbuf_size = 4096; // Why?
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

static esp_err_t msc_storage_mount(void)
{
    esp_err_t ret;
    assert(s_storage_handle);
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

    char drv[3] = {(char)('0' + pdrv), ':', 0};
    ESP_GOTO_ON_ERROR(s_storage_handle->storage->mount(s_storage_handle->context, pdrv), fail, TAG, "Failed pdrv=%d", pdrv);

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
    if (!s_storage_handle) {
        return ESP_FAIL;
    }

    if (s_storage_handle->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB) {
        // If the storage is already mounted to USB, no need to unmount
        return ESP_OK;
    }

    tinyusb_event_cb(TINYUSB_MSC_EVENT_MOUNT_START);

    esp_err_t err = s_storage_handle->storage->unmount(s_storage_handle->context);
    if (err) {
        return err;
    }
    err = esp_vfs_fat_unregister_path(s_storage_handle->fat_fs.base_path);
    s_storage_handle->mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;

    tinyusb_event_cb(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);

    return err;
}

static void msc_mount_event_default_cb(tinyusb_msc_event_t *event)
{
    // Default callback does nothing
    // This is used when no user-defined callback is provided
}

static esp_err_t msc_storage_config(const tinyusb_msc_config_t *config, const storage_api_t *storage_api)
{
    // Default callback
    if (config->callback == NULL) {
        s_storage_handle->event_cb = msc_mount_event_default_cb;
        s_storage_handle->event_arg = NULL;
    } else {
        s_storage_handle->event_cb = config->callback;
        s_storage_handle->event_arg = config->callback_arg;
    }

    s_storage_handle->context = config->context;
    s_storage_handle->storage = storage_api;
    s_storage_handle->mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB; // Default mount point is USB host
    // In case the user does not set mount_config.max_files
    // and for backward compatibility with versions <1.4.2
    // max_files is set to 2
    const int max_files = config->fat_fs.config.max_files;
    s_storage_handle->fat_fs.max_files = max_files > 0 ? max_files : 2;
    s_storage_handle->fat_fs.do_not_format = config->fat_fs.do_not_format;
    s_storage_handle->fat_fs.format_flags = config->fat_fs.format_flags;
    if (s_storage_handle->fat_fs.format_flags == 0) {
        // Use default format flags if not provided
        s_storage_handle->fat_fs.format_flags = FM_ANY; // Auto-select FAT type based on volume size
    }
    if (config->fat_fs.base_path == NULL) {
        // Use default base path if not provided
        s_storage_handle->fat_fs.base_path = TINYUSB_DEFAULT_BASE_PATH;
    } else {
        // Use the provided base path
        s_storage_handle->fat_fs.base_path = config->fat_fs.base_path;
    }

    // Set sector count and size
    storage_info_t storage_info;
    ESP_RETURN_ON_ERROR(s_storage_handle->storage->get_info(s_storage_handle->context, &storage_info), TAG, "Failed to get storage info");
    s_storage_handle->sector_count = storage_info.total_sectors;
    s_storage_handle->sector_size = storage_info.sector_size;

    ESP_LOGD(TAG, "Storage type: , sectors count: %"PRIu32", sector size: %"PRIu32"",
             s_storage_handle->sector_count,
             s_storage_handle->sector_size);

    if (config->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
        // Mount to application
        ESP_RETURN_ON_ERROR(msc_storage_mount(), TAG, "Failed to mount storage to application");
    }

    return ESP_OK;
}

esp_err_t tinyusb_msc_storage_new_spiflash(const tinyusb_msc_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Config can't be NULL");

    assert(!s_storage_handle);
    s_storage_handle = (tinyusb_msc_storage_handle_s *)heap_caps_aligned_calloc(MSC_STORAGE_MEM_ALIGN, sizeof(tinyusb_msc_storage_handle_s), sizeof(uint32_t), MALLOC_CAP_DMA);

    // Init storage based on the config
    const storage_api_t *storage_api = NULL;
    ESP_RETURN_ON_ERROR(storage_spiflash_get(config->context, &storage_api), TAG, "Failed to initialize SPI Flash storage");
    return msc_storage_config(config, storage_api);
}

#if (SOC_SDMMC_HOST_SUPPORTED)
esp_err_t tinyusb_msc_storage_new_mmc(const tinyusb_msc_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Config can't be NULL");

    assert(!s_storage_handle);
    s_storage_handle = (tinyusb_msc_storage_handle_s *)heap_caps_aligned_calloc(MSC_STORAGE_MEM_ALIGN, sizeof(tinyusb_msc_storage_handle_s), sizeof(uint32_t), MALLOC_CAP_DMA);

    // Init storage based on the config
    const storage_api_t *storage_api = NULL;
    ESP_RETURN_ON_ERROR(storage_sdmmc_get(config->context, &storage_api), TAG, "Failed to initialize SPI Flash storage");
    return msc_storage_config(config, storage_api);
}
#endif // SOC_SDMMC_HOST_SUPPORTED

void tinyusb_msc_storage_delete(void)
{
    assert(s_storage_handle);
    if (s_storage_handle->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
        // Unmount the storage if it is mounted to application
        ESP_ERROR_CHECK(msc_storage_unmount());
    }
    heap_caps_free(s_storage_handle);
    s_storage_handle = NULL;
}

uint32_t tinyusb_msc_storage_get_sector_count(void)
{
    assert(s_storage_handle);
    return s_storage_handle->sector_count;
}

uint32_t tinyusb_msc_storage_get_sector_size(void)
{
    assert(s_storage_handle);
    return s_storage_handle->sector_size;
}

tinyusb_msc_mount_point_t tinyusb_msc_storage_get_mount_point(void)
{
    assert(s_storage_handle);
    return s_storage_handle->mount_point;
}

void tinyusb_msc_storage_to_app(void)
{
    if (msc_storage_mount() != ESP_OK) {
        ESP_LOGW(TAG, "Unable to mount storage to app");
        tinyusb_event_cb(TINYUSB_MSC_EVENT_MOUNT_FAILED);
    }
}

void tinyusb_msc_storage_to_usb(void)
{
    if (msc_storage_unmount() != ESP_OK) {
        ESP_LOGW(TAG, "Unable to mount storage to usb");
        tinyusb_event_cb(TINYUSB_MSC_EVENT_MOUNT_FAILED);
    }
}

esp_err_t tinyusb_msc_storage_format_drive(void)
{
    esp_err_t ret;
    // TODO: Revisit the implementation of formatting to verify the collisions and so on
#if (0) // Only for testing purposes
    ESP_RETURN_ON_FALSE(s_storage_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "Storage handle is not initialized");
    // ESP_RETURN_ON_FALSE(s_storage_handle->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP, ESP_ERR_INVALID_STATE, TAG, "Storage must be mounted to application before formatting");
    // Unregister the path before formatting
    esp_vfs_fat_unregister_path(s_storage_handle->fat_fs.base_path);
    // Format the drive
    ESP_RETURN_ON_ERROR(vfs_fat_format(s_storage_handle->fat_fs.format_flags), TAG, "Failed to format the drive");
    // Re-mount the drive after formatting
    // ESP_RETURN_ON_ERROR(vfs_fat_mount(s_storage_handle->fat_fs.base_path, NULL, false), TAG, "Failed to re-mount the drive after formatting"));
    ret = ESP_OK;
#else
    ESP_LOGW(TAG, "Formatting drive is not implemented yet");
    ret = ESP_ERR_NOT_SUPPORTED;
#endif //
    return ret;
}

/* TinyUSB MSC callbacks
   ********************************************************************* */

/** SCSI ASC/ASCQ codes. **/
/** User can add and use more codes as per the need of the application **/
#define SCSI_CODE_ASC_MEDIUM_NOT_PRESENT                0x3A /** SCSI ASC code for 'MEDIUM NOT PRESENT' **/
#define SCSI_CODE_ASC_INVALID_COMMAND_OPERATION_CODE    0x20 /** SCSI ASC code for 'INVALID COMMAND OPERATION CODE' **/
#define SCSI_CODE_ASCQ                                  0x00

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

    if (s_storage_handle->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB) {
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

    uint32_t sec_count = tinyusb_msc_storage_get_sector_count();
    uint32_t sec_size = tinyusb_msc_storage_get_sector_size();
    *block_count = sec_count;
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
        // Eject media
        tinyusb_msc_storage_to_app();
    }
    return true;
}

// Invoked when received SCSI READ10 command
// - Address = lba * BLOCK_SIZE + offset
// - Application fill the buffer (up to bufsize) with address contents and return number of read byte.
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    esp_err_t err = msc_storage_read_sector(lba, offset, bufsize, buffer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_storage_read_sector failed: 0x%x", err);
        return 0;
    }
    return bufsize;
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
static void storage_write_func(void *param)
{
    assert(param); // Ensure param is not NULL
    // Process the data in storage_buffer
    esp_err_t err = msc_storage_write_sector(
                        s_storage_handle->storage_buffer.lba,
                        s_storage_handle->storage_buffer.offset,
                        s_storage_handle->storage_buffer.bufsize,
                        (const void *)s_storage_handle->storage_buffer.data_buffer
                    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Write failed, error=0x%x", err);
    }
}

// Invoked when received SCSI WRITE10 command
// - Address = lba * BLOCK_SIZE + offset
// - Application write data from buffer to address contents (up to bufsize) and return number of written byte.
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    assert(bufsize <= MSC_STORAGE_BUFFER_SIZE);

    // There is no way to return the error from the deferred function, so we need to check everything here
    if (bufsize > MSC_STORAGE_BUFFER_SIZE) {
        ESP_LOGE(TAG, "Buffer size %"PRIu32" exceeds maximum allowed size %d", bufsize, MSC_STORAGE_BUFFER_SIZE);
        goto error;
    }

    size_t addr = 0; // Address of the data to be read, relative to the beginning of the partition.
    size_t temp = 0;
    size_t sector_size = s_storage_handle->sector_size;

    if (__builtin_umul_overflow(lba, sector_size, &temp)) {
        ESP_LOGE(TAG, "WRITE(10): Overflow lba");
        goto error;
    }

    if (__builtin_uadd_overflow(temp, offset, &addr)) {
        ESP_LOGE(TAG, "WRITE(10): Overflow addr");
        goto error;
    }

    if (addr % sector_size != 0 || bufsize % sector_size != 0) {
        ESP_LOGE(TAG, "WRITE(10): Invalid arguments");
        goto error;
    }

    // Copy data to the buffer
    memcpy((void *)s_storage_handle->storage_buffer.data_buffer, buffer, bufsize);
    s_storage_handle->storage_buffer.lba = lba;
    s_storage_handle->storage_buffer.offset = offset;
    s_storage_handle->storage_buffer.bufsize = bufsize;

    // Defer execution of the write to the TinyUSB task
    usbd_defer_func(storage_write_func, (void *)s_storage_handle, false);

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
