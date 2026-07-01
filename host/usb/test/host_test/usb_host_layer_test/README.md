| Supported Targets | Linux |
| ----------------- | ----- |

# USB Host layer Linux host tests

Catch2 + CMock tests for the **USB Host layer** (`usb_host.c`). All layers below the Host layer are mocked via [`usb_host_layer_mock`](../../mocks/usb_host_layer_mock/usb/); the Host layer, `usb_private.c`, and `usb_helpers.c` are built as real code.

FreeRTOS runs as a real component (same pattern as other Linux host tests in this repo).

## Prerequisites

- Linux build host
- ESP-IDF with Linux target support (`idf.py --preview set-target linux`)
- Ruby (required by CMock code generation)

## Test sources

| File                                      | Coverage                                                                |
| ----------------------------------------- | ----------------------------------------------------------------------- |
| `test_main.cpp`                           | Catch2 entry point; runs tests on a FreeRTOS task                       |
| `usb_host_layer_test_fixture.hpp`         | Shared `UsbHostLayerFixture`, install/uninstall helpers, callback flags |
| `usb_host_install_unit_test.cpp`          | Install/uninstall success and failure paths, dual port                  |
| `usb_host_install_config_unit_test.cpp`   | Custom `host_config`, enum filter callback wiring and behavior          |
| `usb_host_client_events_unit_test.cpp`    | Client events                                                           |
| `usb_host_lib_events_unit_test.cpp`       | Library events                                                          |
| `usb_host_usbh_events_unit_test.cpp`      | All USBH injectors                                                      |
| `usb_host_enum_events_unit_test.cpp`      | All enum event injectors                                                |
| `usb_host_hub_events_unit_test.cpp`       | All hub event injectors                                                 |
| `usb_helpers_descriptor_parsing_test.cpp` | Descriptor parsing helpers (`usb_helpers.c`)                            |

## Shared fixture

Event tests use `usb_host_layer_test_fixture.hpp` instead of duplicating install/uninstall CMock expectations.

`UsbHostLayerFixture` wraps `usb_host_install()` / `usb_host_uninstall()` and registers only the mock layer callbacks required by each test:

```cpp
#include "usb_host_layer_test_fixture.hpp"

GIVEN("An installed USB Host library with USBH callbacks registered") {
    usb_host_layer_test::UsbHostLayerFixture host(usb_host_layer_test::MOCK_CB_USBH);
    host.install();

    // use mock injectors from mock_usb_host_layer_test_helpers.h

    usb_host_layer_test::drain_lib_events();
}
```

| Flag           | Use when                                      |
| -------------- | --------------------------------------------- |
| `MOCK_CB_NONE` | Library-only tests (NO_CLIENTS, AUTO_SUSPEND) |
| `MOCK_CB_USBH` | USBH injectors and client event tests         |
| `MOCK_CB_ENUM` | Enum event injectors                          |
| `MOCK_CB_HUB`  | Hub event injectors                           |
| `MOCK_CB_ALL`  | Proc-req routing (USBH + Hub + Enum)          |

Pass a custom `usb_host_config_t` as the second constructor argument for `skip_phy_setup`, `root_port_unpowered`, multi-port `peripheral_map`, or `enum_filter_cb`. PHY and `hub_root_start` expectations follow the config automatically.

See [`usb_host_layer_mock` README](../../mocks/usb_host_layer_mock/usb/README.md) for the full mock API (injectors, manual wiring, and side-effect expectations).

## Configuration

`sdkconfig.defaults` in this directory sets:

```ini
CONFIG_IDF_TARGET="linux"
CONFIG_COMPILER_CXX_EXCEPTIONS=y
CONFIG_UNITY_ENABLE_IDF_TEST_RUNNER=n
CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK=y
```

`CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK` is defined in the mock component's `Kconfig` (the Linux target does not expose the full USB-OTG menu from the main `usb` component).

## Build

```bash
cd host/usb/test/host_test/usb_host_layer_test
idf.py --preview set-target linux
idf.py build
```

## Run

The build produces `build/host_test_usb_host_layer.elf`.

Default run (summary only):

```bash
./build/host_test_usb_host_layer.elf
```

Expected: **22 test cases**, all passing.

Verbose Catch2 output (sections, passed assertions, timings):

```bash
./build/host_test_usb_host_layer.elf -v high -s -d yes
```

List or filter tests:

```bash
./build/host_test_usb_host_layer.elf --list-tests
./build/host_test_usb_host_layer.elf -v high -s "Scenario: USB Host client NEW_DEV event routing"
./build/host_test_usb_host_layer.elf --help
```

Alternatively:

```bash
idf.py monitor
```

**Note:** `E (...) USB HOST:` log lines during a full run are expected. They come from negative install/error-path tests, not failures.
