# MSC + NCM async storage IO example

A USB composite device with a USB drive (MSC) and a USB Ethernet adapter (CDC-NCM). It shows what `CONFIG_TINYUSB_MSC_ASYNC_IO` does to a network interface on the same device. A script on the host measures network latency and throughput with and without a copy to the drive.

Without async IO, storage reads and writes run in the TinyUSB task. NCM uses the same task in both directions:

- Receive: TinyUSB passes incoming frames to lwIP from the TinyUSB task.
- Transmit: `tinyusb_net_send_sync()` hands each frame to the TinyUSB task and waits for it. While the TinyUSB task is busy with storage, the lwIP task waits too, and so does every socket on the device.

With async IO, storage IO runs in a separate worker task and the TinyUSB task keeps serving NCM.

## Hardware

ESP32-P4 with a microSD card, for example the ESP32-P4-Function-EV-Board. Connect the USB OTG (high speed) port to the host. Other chips with USB OTG work with internal flash as storage. ESP-IDF 6.0.4 or newer.

The SD card pins and LDO channel are in **MSC + NCM async IO example → SD card slot** in `idf.py menuconfig`. The device does not mount or format the card. Use a scratch card formatted FAT32 on the host.

## Build and flash

Without async IO:

```bash
idf.py set-target esp32p4
idf.py build flash monitor
```

With async IO:

```bash
idf.py -B build_async -D SDKCONFIG=build_async/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.async" build flash monitor
```

The UART console prints the storage rate every second while the host uses the drive.

## What the device provides

| Service     | Address                     | Used for                                                                                     |
| ----------- | --------------------------- | -------------------------------------------------------------------------------------------- |
| DHCP server | 192.168.4.1                 | Gives the host an address in 192.168.4.x                                                     |
| UDP echo    | port 7                      | Round-trip latency                                                                           |
| TCP sink    | port 5001                   | Upload throughput (host to device). Also works with an iperf2 client: `iperf -c 192.168.4.1` |
| TCP source  | port 5002                   | Download throughput (device to host)                                                         |
| HTTP        | `http://192.168.4.1/status` | Async IO mode, storage byte counters, NCM TX drops (JSON)                                    |

`ncm_tx_drops` counts frames lwIP sent that USB did not accept within 100 ms. TCP retransmits them.

## Measure

1. Plug in the device. On macOS a new network interface appears (System Settings → Network) and gets an address by DHCP. Check with:

   ```bash
   curl http://192.168.4.1/status
   ```

2. Note the mount point of the USB drive, for example `/Volumes/NO NAME`.

3. Run the script (Python 3, standard library only):

   ```bash
   python3 tools/measure.py --volume "/Volumes/NO NAME"
   ```

   It writes and deletes a 100 MB test file on the drive several times, and runs these phases:

   | Phase                            | What runs                                    |
   | -------------------------------- | -------------------------------------------- |
   | `copy`                           | Copy only: the MSC write rate alone          |
   | `latency_idle`, `latency_copy`   | UDP echo every 10 ms, idle and during a copy |
   | `tcp_up_idle`, `tcp_up_copy`     | TCP upload, idle and during a copy           |
   | `tcp_down_idle`, `tcp_down_copy` | TCP download, idle and during a copy         |

   The results are printed and appended to `results.csv`, labelled with the mode reported by the device.

4. Flash the other build and run the script again. Compare the `*_copy` rows of the two builds.

What to look at:

- `rtt_max_ms`, `rtt_p99_ms` and `rtt_over_10ms` in `latency_copy`: without async IO, round trips wait for the storage access in progress.
- `tcp_*_min_1s_mbps` in `tcp_*_copy`: the worst second of throughput during the copy.
- `copy_mbps` in the `*_copy` phases compared to `copy`: what the network traffic costs the drive. Both classes share the same USB bus, so some slowdown is expected even with async IO.

Useful options: `--size-mb` for longer copies, `--tcp up|down|none` to skip phases, `--idle-s` for longer baselines.

A fast SD card may show only short stalls without async IO. To make the medium slower, set **MSC + NCM async IO example → Simulated slow medium delay per storage access**.

## Internal flash

Flash writes and erases disable the cache on all cores, so all tasks that run from flash stop, including the TinyUSB and lwIP tasks. Async IO cannot avoid that, so a few stalls remain with internal flash.

## Files

- `main/msc_ncm_main.c`: USB setup and the storage rate log.
- `main/storage.c`: SD card or flash partition, wrapped by a block device that counts bytes and adds the optional delay.
- `main/ncm_netif.c`: lwIP interface on top of NCM, adapted from ESP-IDF `examples/network/sta2eth`.
- `main/net_servers.c`: UDP echo, TCP sink and source, HTTP status.
- `tools/measure.py`: host-side measurement.
