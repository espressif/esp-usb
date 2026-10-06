# MSC async IO test app

Tests `CONFIG_TINYUSB_MSC_ASYNC_IO` against a USB host.

The firmware is a CDC + MSC composite device:

- **MSC** exposes a RAM disk through `esp_blockdev`. The RAM disk adds a configurable delay to every read and write, to emulate slow media, and can make writes fail on request.
- **CDC** echoes every packet from the RX callback, which runs in the TinyUSB task. Echo latency shows how long the TinyUSB task was blocked.
- **Control packets** on the CDC port (first byte `0xFF`):

| Request              | Reply                                                                                          |
| -------------------- | ---------------------------------------------------------------------------------------------- |
| `FF 'D' <ms u16 LE>` | `D <ms>\n` – set the storage delay                                                             |
| `FF 'S'`             | `S <reads> <writes> <delay_ms>\n` – storage access counters                                    |
| `FF 'R'`             | `R\n` – reset the counters                                                                     |
| `FF 'F' <n u16 LE>`  | `F <n>\n` – the next n storage writes fail                                                     |
| `FF 'I'`             | `I <io_mode>\n` – `sync` or `async`                                                            |
| `FF 'W'`             | `W <used> <size>\n` – peak stack use and stack size of the async IO worker, `-1 -1` without it |

The RAM disk is formatted with FAT at boot, so the host mounts it as a normal drive. Its size is `CONFIG_TEST_RAMDISK_SIZE_KB`: 4 MB in PSRAM when `CONFIG_SPIRAM` is enabled, otherwise 192 KB of internal RAM. The ESP32-P4 defaults enable `CONFIG_SPIRAM`, so the ESP32-P4 build needs a board with PSRAM; without it, the RAM disk allocation fails at boot.

## Host check script

`check_msc_async_io.py` runs on macOS and Linux. It uses the CDC port and the MSC drive mounted by the host OS, so it needs no root access and no raw block device access. It checks that:

1. A file written to the MSC drive reads back unchanged (SHA-256). The host page cache is bypassed, so every write and read reaches the device.
2. CDC echo latency while the MSC drive is busy, for each storage delay: with async IO it stays below a quarter of the delay, without async IO it grows above half of the delay. The second condition confirms the check measures the effect.
3. With `--inject-errors`: one failing storage write is reported to the host as an I/O error with async IO. Without async IO the error is only in the device log. If the host saw no error, the file is read back: a mismatch shows the failed write was lost without the host knowing.

It exits with 0 when all checks pass. The test is not run in CI, because it needs the MSC drive mounted by the host.

Requirements: ESP-IDF >= 6.0.4 (esp_blockdev support in esp_tinyusb MSC) and `pyserial` on the host (included in the ESP-IDF Python environment).

## Build and flash

Build both configurations, async IO off and on:

```sh
idf.py -B build_sync -DSDKCONFIG=build_sync/sdkconfig set-target esp32p4 build
idf.py -B build_async -DSDKCONFIG=build_async/sdkconfig \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.async" \
       set-target esp32p4 build
```

Flash one of them through the UART/JTAG port and check the log:

```sh
idf.py -B build_async -p <PORT> flash monitor
```

```
I (...) msc_async_test: Ready. MSC IO mode: async, added storage delay: 0 ms
```

Connect the native USB port (USB-OTG HS on ESP32-P4) to the host. A FAT drive and a CDC serial port appear.

## Run

The script and its options are the same on both systems. Only the two paths differ:

|            | macOS                                       | Linux                                                                        |
| ---------- | ------------------------------------------- | ---------------------------------------------------------------------------- |
| `--port`   | Appears on connect: `ls /dev/cu.usbmodem*`  | Appears on connect: `ls /dev/ttyACM*`                                        |
| `--volume` | `"/Volumes/NO NAME"`, mounted automatically | Mount point of the drive, see below. Find it with `lsblk -o NAME,MOUNTPOINT` |

macOS:

```sh
python check_msc_async_io.py --port <CDC port> --volume "/Volumes/NO NAME"
python check_msc_async_io.py --port <CDC port> --volume "/Volumes/NO NAME" --inject-errors
```

Linux:

```sh
python check_msc_async_io.py --port <CDC port> --volume /media/$USER/<label>
python check_msc_async_io.py --port <CDC port> --volume /media/$USER/<label> --inject-errors
```

On Linux:

- If the drive is not mounted automatically, mount its first partition, for example `/dev/sdX1`. The firmware formats the drive with a partition table.
- ModemManager may probe a new `ttyACM` port for a few seconds after it appears and disturb the echo. Wait until it is done, or stop ModemManager while the test runs.

Windows is not supported: the script uses POSIX file calls to bypass the host cache.

Then flash the other build and run again.

- Run `--inject-errors` last. After the I/O error the host may eject the drive (macOS) or remount it read-only (Linux). Reconnect the device afterwards.
- Without PSRAM the drive is only 192 KB. Use a smaller file, for example `--file-kb 64`.
- Other options: `--delays 0,5,20`, `--duration 5`, `--probe-hz 200`.

The script prints one idle row, then one load row per delay:

```
delay phase |   p50 ms   p90 ms   max ms      n  tmo |  wr MB/s  rd MB/s rounds verify  MSC rd/wr ops | result
```

- The **idle** row is the baseline: echo probes only, no storage access. It should be the same for both builds.
- **load** rows show the effect: echo latency while the drive is written and read back.
- `verify` must be `OK` in every load row.
- `MSC rd/wr ops` counts storage accesses during the phase. Zero writes in a load row means the host did not reach the device, for example because the wrong volume was given.
- The last line shows the peak stack use of the async IO worker task and its stack size.
