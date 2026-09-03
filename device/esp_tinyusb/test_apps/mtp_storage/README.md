# TinyUSB MTP storage test

The default test run covers the Unity cases and basic Linux GVfs access. Install `usbutils`, `dbus`, `gvfs-backends`, and `libglib2.0-bin`, connect one supported target, then run from this directory:

```sh
export XDG_RUNTIME_DIR=/tmp/xdg-runtime
install -d -m 700 "$XDG_RUNTIME_DIR"
dbus-run-session -- pytest --target esp32s2 -m usb_device
```

Run only the basic host access test with:

```sh
dbus-run-session -- pytest --target esp32s2 pytest_mtp_storage.py::test_usb_device_mtp_manual_pc_access
```

Performance, concurrency, and many-file tests are opt-in. Enable them explicitly and select the required test:

```sh
MTP_RUN_EXTENDED_TESTS=1 dbus-run-session -- pytest --target esp32s2 pytest_mtp_storage.py::test_usb_device_mtp_host_performance
MTP_RUN_EXTENDED_TESTS=1 dbus-run-session -- pytest --target esp32s2 pytest_mtp_storage.py::test_usb_device_mtp_host_concurrency
MTP_RUN_EXTENDED_TESTS=1 dbus-run-session -- pytest --target esp32s2 pytest_mtp_storage.py::test_usb_device_mtp_host_many_files
```

Set `MTP_TEST_USB_ID` when the firmware uses a VID:PID other than `303a:4040`. Only one matching MTP device may be connected.
