# USB: Host test application

The tests in this application expect one of the following USB devices:

1. Real USB device: full-speed or high-speed USB flash disk with 2 bulk endpoints, connected to the USB Host DUT.
   Used by the majority of the test cases (`full_speed` and `high_speed` test groups).
2. Mock HID device: composite HID mouse emulated by the TinyUSB device stack running on a second ESP board
   (see [mock_hid.c](../common/mock_hid.c)), connected to the USB Host DUT. Both boards run this application: the mock
   board runs the mock device test case, the DUT runs the host pipe test cases.

The test cases are implemented in the following files:

- [`test_hcd_intr.c`](main/test_hcd_intr.c): Tests for interrupt transfers.
- [`test_hcd_ctrl.c`](main/test_hcd_ctrl.c): Tests for control transfers.
- [`test_hcd_bulk.c`](main/test_hcd_bulk.c): Tests for bulk transfers.
- [`test_hcd_isoc.c`](main/test_hcd_isoc.c): Tests for isochronous transfers.
- [`test_hcd_port.c`](main/test_hcd_port.c): Tests for root port operations.
- [`test_hcd_public_api.c`](main/test_hcd_public_api.c): Tests for HCD public API.
- [`test_hcd_xfer_size_limit.c`](main/test_hcd_xfer_size_limit.c): Tests for transfer size limits.

For running the tests with a real USB device locally, you will have to update
device definitions (VID, PID, ...) in [dev_msc.c](../common/dev_msc.c) and
[dev_isoc.c](../common/dev_isoc.c).
