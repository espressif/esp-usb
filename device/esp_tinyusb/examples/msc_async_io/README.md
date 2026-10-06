# MSC async storage IO example

A USB composite device with a USB drive (MSC) and a serial port (CDC-ACM). It shows what `CONFIG_TINYUSB_MSC_ASYNC_IO` changes: without it, storage reads and writes run in the TinyUSB task, and other USB classes on the device stop responding while the storage is busy.

The USB drive is a microSD card on ESP32-P4, or the `storage` partition in flash (2 MB, FAT, wear levelled) on other chips. Over the serial port, the device prints a line every 100 ms with the USB latency: the time from queuing the line until TinyUSB reports it sent. While the TinyUSB task is busy with storage IO, this time grows by the length of the storage access.

## Hardware

Any ESP chip with USB OTG (ESP32-S2, ESP32-S3, ESP32-P4, ...). Connect the USB OTG port to the host. ESP-IDF 6.0.4 or newer.

On ESP32-P4 the example uses an SD card by default. The default pins and the on-chip LDO channel match the ESP32-P4-Function-EV-Board; change them in **MSC async IO example → SD card slot** in `idf.py menuconfig`. The device does not mount or format the card, the host sees it as it is. Use a scratch card, formatted FAT32 on the host.

To use internal flash instead, select **MSC async IO example → Storage exposed over USB → Internal flash**. Other chips use internal flash by default.

## Build and flash

Without async IO (default):

```bash
idf.py set-target esp32p4
idf.py build flash monitor
```

With async IO:

```bash
idf.py -B build_async -D SDKCONFIG=build_async/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.async" build flash monitor
```

`idf.py monitor` shows the UART console, not the USB serial port.

## Try it

1. Open the USB serial port of the device in a terminal, for example `screen /dev/ttyACM0` or PuTTY on Windows. The device prints:
   ```
   [async IO: OFF] usb latency    0.2 ms
   [async IO: OFF] usb latency    0.3 ms
   ```
2. Copy a file of a few MB to the USB drive. With internal flash, the host may format the drive on first use.
3. Watch the terminal. Without async IO, the output stops for a moment and then continues in bursts. Every second, a progress line shows the storage read and write rate, the totals so far and the worst USB latency in that second:
   ```
   --- MSC activity started (async IO OFF) ---
   [async IO: OFF] usb latency   48.6 ms   <-- STALL
   [async IO: OFF] usb latency   51.0 ms   <-- STALL
   [msc] write  16.8 MB/s  read   0.0 MB/s | total W    16.9 MB  R     0.1 MB | usb latency max  67.0 ms
   ```
4. One second after the storage goes idle, the device prints a summary:
   ```
   --- MSC activity done (async IO OFF): 6.2 s, wrote 104.3 MB (16.8 MB/s), read 0.4 MB (0.1 MB/s) ---
   --- usb latency max 67.0 ms, avg 12.1 ms, stalls >10 ms: 52/62 ---
   ```
5. Flash the async IO build and copy the same file. With an SD card, the latency stays low during the copy and the summary reports no stalls. Compare the write rate to see what async IO costs or saves in throughput.

The numbers above are illustrative; they depend on the chip, card and host.

For long copies, enable **MSC async IO example → Print only stalled heartbeat lines**. The latency is still measured every 100 ms, but only stalls, progress and summary lines are printed.

## What the device sees

The device does not see files. The host's file system turns a copy into SCSI commands, carried by the Bulk-Only Transport protocol:

1. The host sends a command, for example "WRITE10, 256 blocks at LBA 81920".
2. The data follows in chunks of `CONFIG_TINYUSB_MSC_BUFSIZE`. TinyUSB calls the write callback once per chunk, and that is where the storage is accessed.
3. The device returns a status, and the host sends the next command.

Between data runs, the host also writes file system metadata (FAT, directory entries) with the same commands. "MSC activity" in the output therefore means any storage access by the host, including mounting the drive or opening a folder. Hosts also cache writes and may write a copied file later or in bursts, so the activity does not exactly match the copy dialog on the host. The rates count the bytes passed to the storage, metadata included.

## Internal flash and slow media

With internal flash, a few stalls remain even with async IO. Flash writes and erases disable the cache on all cores, so every task that runs from flash, including the TinyUSB task, stops until the operation finishes. Async IO cannot avoid that. An SD card does not have this limitation.

A fast SD card may show only small stalls without async IO. To make any medium slower, set **MSC async IO example → Simulated slow medium delay per storage access**. Every storage access then sleeps that long.

## How it works

- The SD card (`sdmmc_get_blockdev()`) or the `storage` partition (`esp_partition_get_blockdev()` and `wl_get_blockdev()`) is opened as a block device and wrapped by a small block device that counts the bytes read and written (for the progress and summary lines) and adds the optional delay. The wrapper is passed to `tinyusb_msc_new_storage_blockdev()`.
- The heartbeat task writes a line over CDC and waits for `tud_cdc_tx_complete_cb()`. TinyUSB calls it from the TinyUSB task, so the wait includes any time that task spent on storage IO.
- Async IO needs no application code. With `CONFIG_TINYUSB_MSC_ASYNC_IO=y`, esp_tinyusb runs storage IO in its own worker task. See the [esp_tinyusb README](../../README.md#asynchronous-storage-io) for the options and RAM cost.
