/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "usb/msc_host.h"

#ifdef MSC_HOST_BDL_API_SUPPORTED

#include <stdlib.h>
#include <catch2/catch_test_macros.hpp>

#include "../../private_include/msc_common.h"

namespace {

// Builds a bare msc_device_t with just enough state (disk geometry) for
// msc_host_get_blockdev() to operate on. The BDL adapter only reads
// dev->disk, so the rest of msc_device_t can stay zeroed.
msc_device_t *make_fake_device(uint32_t block_size, uint32_t block_count)
{
    msc_device_t *dev = (msc_device_t *)calloc(1, sizeof(msc_device_t));
    dev->disk.block_size = block_size;
    dev->disk.block_count = block_count;
    return dev;
}

} // namespace

SCENARIO("msc_host_get_blockdev argument validation")
{
    GIVEN("A valid fake MSC device") {
        msc_device_t *dev = make_fake_device(512, 1000);
        esp_blockdev_handle_t handle = ESP_BLOCKDEV_HANDLE_INVALID;

        SECTION("NULL device is rejected") {
            REQUIRE(ESP_ERR_INVALID_ARG == msc_host_get_blockdev(NULL, &handle));
        }

        SECTION("NULL out_handle is rejected") {
            REQUIRE(ESP_ERR_INVALID_ARG == msc_host_get_blockdev((msc_host_device_handle_t)dev, NULL));
        }

        SECTION("Both NULL is rejected") {
            REQUIRE(ESP_ERR_INVALID_ARG == msc_host_get_blockdev(NULL, NULL));
        }

        free(dev);
    }
}

SCENARIO("msc_host_get_blockdev geometry derivation")
{
    GIVEN("A fake MSC device with known block size and count") {
        const uint32_t block_size = 512;
        const uint32_t block_count = 2048;
        msc_device_t *dev = make_fake_device(block_size, block_count);

        WHEN("a BDL handle is requested") {
            esp_blockdev_handle_t handle = ESP_BLOCKDEV_HANDLE_INVALID;
            REQUIRE(ESP_OK == msc_host_get_blockdev((msc_host_device_handle_t)dev, &handle));

            THEN("the handle is valid and its geometry matches disk block size/count") {
                REQUIRE(handle != ESP_BLOCKDEV_HANDLE_INVALID);
                REQUIRE(handle->geometry.disk_size == (uint64_t)block_count * block_size);
                REQUIRE(handle->geometry.read_size == block_size);
                REQUIRE(handle->geometry.write_size == block_size);
                REQUIRE(handle->geometry.erase_size == 0);
            }

            REQUIRE(ESP_OK == msc_host_release_blockdev(handle));
        }

        free(dev);
    }
}

SCENARIO("msc_host_get_blockdev is a factory, not an accessor")
{
    GIVEN("A fake MSC device") {
        msc_device_t *dev = make_fake_device(512, 1000);

        WHEN("msc_host_get_blockdev is called twice") {
            esp_blockdev_handle_t handle1 = ESP_BLOCKDEV_HANDLE_INVALID;
            esp_blockdev_handle_t handle2 = ESP_BLOCKDEV_HANDLE_INVALID;
            REQUIRE(ESP_OK == msc_host_get_blockdev((msc_host_device_handle_t)dev, &handle1));
            REQUIRE(ESP_OK == msc_host_get_blockdev((msc_host_device_handle_t)dev, &handle2));

            THEN("two independent handles are returned") {
                REQUIRE(handle1 != ESP_BLOCKDEV_HANDLE_INVALID);
                REQUIRE(handle2 != ESP_BLOCKDEV_HANDLE_INVALID);
                REQUIRE(handle1 != handle2);
            }

            REQUIRE(ESP_OK == msc_host_release_blockdev(handle1));
            REQUIRE(ESP_OK == msc_host_release_blockdev(handle2));
        }

        free(dev);
    }
}

SCENARIO("msc_host_release_blockdev ownership checks")
{
    GIVEN("NO handle (NULL)") {
        SECTION("Releasing NULL is rejected") {
            REQUIRE(ESP_ERR_INVALID_ARG == msc_host_release_blockdev(NULL));
        }
    }

    GIVEN("A handle not created by msc_host_get_blockdev") {
        // Same shape as a real esp_blockdev_t, but with a foreign ops table.
        // msc_host_release_blockdev() must reject it rather than calling
        // through a mismatched/foreign ops->release.
        static const esp_blockdev_ops_t foreign_ops = {};
        esp_blockdev_t foreign_handle = {};
        foreign_handle.ops = &foreign_ops;

        SECTION("It is rejected instead of being released") {
            REQUIRE(ESP_ERR_INVALID_ARG == msc_host_release_blockdev(&foreign_handle));
        }
    }

    GIVEN("A handle created by msc_host_get_blockdev") {
        msc_device_t *dev = make_fake_device(512, 1000);
        esp_blockdev_handle_t handle = ESP_BLOCKDEV_HANDLE_INVALID;
        REQUIRE(ESP_OK == msc_host_get_blockdev((msc_host_device_handle_t)dev, &handle));

        SECTION("It is released successfully") {
            REQUIRE(ESP_OK == msc_host_release_blockdev(handle));
        }

        free(dev);
    }
}

SCENARIO("msc_host BDL ops contract")
{
    GIVEN("A BDL handle obtained from msc_host_get_blockdev") {
        msc_device_t *dev = make_fake_device(512, 1000);
        esp_blockdev_handle_t handle = ESP_BLOCKDEV_HANDLE_INVALID;
        REQUIRE(ESP_OK == msc_host_get_blockdev((msc_host_device_handle_t)dev, &handle));

        THEN("sync always succeeds (BOT/SCSI has no host-visible write cache)") {
            REQUIRE(ESP_OK == handle->ops->sync(handle));
        }

        THEN("erase is not implemented (USB MSC is not NOR/NAND)") {
            REQUIRE(handle->ops->erase == NULL);
        }

        THEN("ioctl reports not supported, so FatFS TRIM treats it as a no-op success") {
            REQUIRE(ESP_ERR_NOT_SUPPORTED == handle->ops->ioctl(handle, 0, NULL));
        }

        REQUIRE(ESP_OK == msc_host_release_blockdev(handle));
        free(dev);
    }
}

#endif // MSC_HOST_BDL_API_SUPPORTED
