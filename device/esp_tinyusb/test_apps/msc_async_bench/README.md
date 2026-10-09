# MSC async IO benchmark

Measures how much MSC storage IO delays the other USB classes of the same device, with `CONFIG_TINYUSB_MSC_ASYNC_IO` off and on.

All USB classes in esp_tinyusb are serviced by one TinyUSB task. Without async IO, every MSC READ10/WRITE10 chunk runs the storage read/write inside that task, so CDC, HID and other classes wait until it returns. With async IO, the storage access runs in a separate worker task and the TinyUSB task keeps servicing the other classes.

## How it works

The firmware is a CDC + MSC composite device:

- **MSC** exposes a block device through `esp_blockdev`:

  - a RAM disk (default, PSRAM if available, otherwise internal RAM), or
  - a microSD card on the ESP32-P4-Function-EV-Board, see [SD card](#sd-card).

  The RAM disk is used instead of internal SPI flash because flash writes disable the cache and stall non-IRAM tasks on all cores, which would hide the effect being measured.

  The block device is wrapped by a pass-through block device that adds an optional delay per read and write (to emulate slow media), read/write counters and write failure injection. These work the same for both storage types.

- **CDC** echoes every packet directly from the RX callback, which runs in the TinyUSB task. Echo latency therefore shows how long the TinyUSB task was busy.

- **Control packets** on the CDC port (first byte `0xFF`) change the storage delay at runtime, so one flash covers a sweep of delays:

| Request              | Reply                                                                                                                             |
| -------------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| `FF 'D' <ms u16 LE>` | `D <ms>\n` – set added storage delay                                                                                              |
| `FF 'S'`             | `S <reads> <writes> <delay_ms>\n`                                                                                                 |
| `FF 'R'`             | `R\n` – reset read/write counters                                                                                                 |
| `FF 'F' <n u16 LE>`  | `F <n>\n` – the next n storage writes fail                                                                                        |
| `FF 'I'`             | `I <storage> <io_mode> <sd_khz>\n` – flashed configuration, e.g. `I sd async 40000`                                               |
| `FF 'W'`             | `W <used> <size>\n` – peak stack use of the async IO worker task since boot and its stack size in bytes, `-1 -1` without async IO |

`bench.py` runs on the host. For each delay it measures CDC echo round-trip time twice:

- **idle**: echo probes only
- **load**: echo probes while a second thread repeatedly writes a random file to the MSC drive, reads it back with the page cache bypassed, and compares SHA-256

## Requirements

- ESP-IDF >= 6.0.4 (esp_blockdev support in esp_tinyusb MSC)
- `pyserial` on the host (included in the ESP-IDF Python environment)

The RAM disk is 4 MB in PSRAM when `CONFIG_SPIRAM` is enabled (the ESP32-P4 defaults enable it), otherwise 192 KB of internal RAM (`CONFIG_BENCH_RAMDISK_SIZE_KB`). The app builds for every target with USB-OTG but has only been run on ESP32-P4. On full-speed targets (ESP32-S2/S3/H4) the MSC buffer is 512 B, so absolute numbers are not comparable with ESP32-P4; the sync/async comparison still applies. On ESP32-S2, 192 KB may not fit in internal RAM.

## Build and flash

Two variants are available:

| Build dir     | sdkconfig file       | Behaviour                                                                                                                       |
| ------------- | -------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| `build_sync`  | (defaults)           | Storage IO in the TinyUSB task. Writes are accepted before they are written; write errors are only logged.                      |
| `build_async` | `sdkconfig.ci.async` | Storage IO in a worker task. A write chunk is accepted while the write buffer is free; errors are reported on the next command. |

```sh
idf.py -B build_sync  -DSDKCONFIG=build_sync/sdkconfig  set-target esp32p4 build
idf.py -B build_async -DSDKCONFIG=build_async/sdkconfig \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.async" \
       set-target esp32p4 build
```

Flash one of them through the UART/JTAG port:

```sh
idf.py -B build_sync -p <UART_PORT> flash monitor
```

The log shows which variant is running:

```
I (...) msc_bench: Ready. Storage: ramdisk, MSC IO mode: sync, added storage delay: 0 ms
```

Connect the native USB port (USB-OTG HS on ESP32-P4) to the host. A drive (FAT, about 4 MB) and a CDC serial port appear.

### SD card

On the ESP32-P4-Function-EV-Board, the microSD slot can be used instead of the RAM disk. Add `sdkconfig.bench_sd` to any variant:

```sh
idf.py -B build_sd_async -DSDKCONFIG=build_sd_async/sdkconfig \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.async;sdkconfig.bench_sd" \
       set-target esp32p4 build
```

For the sync variant use `"sdkconfig.defaults;sdkconfig.bench_sd"`.

- The card is accessed with 4-bit SDMMC at 40 MHz (`CONFIG_BENCH_SD_HIGHSPEED`), powered by on-chip LDO channel 4, and exposed with `sdmmc_get_blockdev()`.
- The firmware never mounts or formats the card. Only the host uses its filesystem.
- Use a scratch card formatted FAT32 on the host. `--inject-errors` can corrupt the filesystem.
- Run with `--delays 0`. The added delay is applied on top of the card's own access time, so only `delay_ms = 0` rows measure the card alone. Rows with a non-zero delay behave like a slower card. Use a longer run and a larger file so that the card's internal erase and housekeeping pauses appear in the results, for example `--duration 30 --file-kb 8192`.
- If a buffer passed to the SD driver is not cache-line aligned, the log shows `... buffer ... is not 64-byte aligned` once. The SD driver then copies through a bounce buffer, which slows down that variant.

## Run

```sh
python bench.py --port /dev/cu.usbmodem<N> --volume "/Volumes/NO NAME" --label sync  --csv results.csv
# flash the next variant, reconnect, and repeat with --label async
```

Useful options: `--delays 0,5,20`, `--duration 10`, `--file-kb 2048`, `--probe-hz 500`.

The script prints one row per delay and phase:

```
delay phase |   p50 ms   p99 ms   max ms      n  tmo |  wr MB/s  rd MB/s rounds verify  MSC rd/wr ops
```

### Write error reporting

```sh
python bench.py --port /dev/cu.usbmodem<N> --volume "/Volumes/NO NAME" --inject-errors
```

Makes the next storage write fail, writes a file with a full sync, then writes a second file, and prints where the host saw an I/O error. Expected:

- sync: no error on either write. The failure is only in the device log.
- async: error on the first file (at write or fsync) or, if the failed chunk was the last one of its command, on the second write.

After an I/O error the host may remount or eject the volume. Run this last, or reconnect the device afterwards.

## Reading the results

- **idle** rows are the baseline and should be the same for both variants.
- **load** rows show the effect. Without async IO, echo latency under load grows with the storage delay, because each echo can wait behind a full storage access. With async IO it should stay close to the idle baseline.
- `wr MB/s` / `rd MB/s` show whether async IO costs throughput.
- `verify` must be `OK` in every load row. `FAIL` means data written over MSC did not read back correctly.
- `MSC rd/wr ops` counts storage accesses during the phase. Zero in a load row means the host did not touch the device (for example, the wrong volume was given).
- CSV columns `storage` (`ramdisk` / `sd`), `io_mode` (`sync` / `async`) and `sd_khz` are read from the device with the `I` request, so each row records what was actually flashed. `delay_ms` is the added delay only. When appending to a CSV written by an older `bench.py`, the file is rewritten with the new columns, left empty for the old rows.
- `stack used B` (CSV `worker_stack_used`, `worker_stack_size`) is the peak stack use of the async IO worker since boot, shown as used/size in bytes with the percentage, for example `1832/4096 (45%)`. The size is `CONFIG_TINYUSB_MSC_ASYNC_IO_TASK_STACK_SIZE`. The value never goes down during a boot, so the last row of a run and the line printed by `--inject-errors` show the worst case so far. `--inject-errors` reaches the error logging in the worker, which uses more stack than successful IO. The worker's stack use also depends on the storage driver, so measure with the storage used in the product (the SD card path is deeper than the RAM disk).
- On macOS, `F_NOCACHE` and `F_FULLFSYNC` keep the host page cache out of the measurement. On Linux the script falls back to `fsync` + `posix_fadvise(DONTNEED)`.
