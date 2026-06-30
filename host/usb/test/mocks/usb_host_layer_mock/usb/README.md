# USB Host layer mock

This component mocks all USB stack layers **below** the USB Host layer (`usb_host.c`). The Host layer itself is built as a real component. That makes it possible to run Linux host tests (Catch2 + CMock) against the real Host-layer logic while USBH, Enum, Hub, HCD, and PHY are mocked.

## Layout

| Path                                             | Visibility  | Role                                                                                     |
| ------------------------------------------------ | ----------- | ---------------------------------------------------------------------------------------- |
| `CMakeLists.txt`                                 | —           | Builds the mock component; links real `usb_host.c`, `usb_private.c`, and `usb_helpers.c` |
| `Kconfig`                                        | —           | Mock-specific Kconfig options (see [Configuration](#configuration))                      |
| `mock/mock_config.yaml`                          | —           | CMock plugins enabled for generated mocks                                                |
| `include/mock_usb_host_layer_callbacks.h`        | **Public**  | CMock capture hooks (`usbh_register_mock_callback`, …)                                   |
| `include/mock_usb_host_layer_events.h`           | **Public**  | Event and processing-request injectors                                                   |
| `include/mock_usb_host_layer_test_helpers.h`     | **Public**  | Umbrella header that includes both public headers                                        |
| `private_include/mock_usb_host_layer_internal.h` | **Private** | Shared callback storage between the two `.c` files                                       |
| `mock_usb_host_layer_callbacks.c`                | —           | Callback capture implementation                                                          |
| `mock_usb_host_layer_events.c`                   | —           | Event/proc-req injection implementation                                                  |

Consumer tests live in [`host/usb/test/host_test/usb_host_layer_test/`](../../host_test/usb_host_layer_test/).

### Public vs private headers

Headers under `include/` are exported to anything that `REQUIRES` this mock component (host-layer tests). That is the intended test API.

`private_include/` holds implementation details shared only between `mock_usb_host_layer_callbacks.c` and `mock_usb_host_layer_events.c`. It is wired in CMake via `target_include_directories(... PRIVATE "private_include")` and must **not** be included from test code.

Tests should use:

```cpp
#include "mock_usb_host_layer_test_helpers.h"   // or the two public headers directly
```

Not `mock_usb_host_layer_internal.h`.

## Configuration

The Linux target does not expose the full USB-OTG Kconfig menu from the main `usb` component. Options needed by this mock are duplicated in `Kconfig`:

| Option                                 | Purpose                                                                                             |
| -------------------------------------- | --------------------------------------------------------------------------------------------------- |
| `USB_HOST_CONTROL_TRANSFER_MAX_SIZE`   | Control transfer buffer size used by the real Host layer                                            |
| `USB_HOST_ENABLE_ENUM_FILTER_CALLBACK` | Enables `enum_filter_cb` in `enum_config_t` and `usb_host_config_t`; required for enum filter tests |

Enable options in the test project's `sdkconfig.defaults` or `menuconfig`. The host-layer test app sets `CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK=y` by default.

## Test helpers

These helpers solve a specific problem in host-layer unit tests: **`usb_host.c` registers callbacks on the mocked lower layers during `usb_host_install()`, but tests have no direct access to those function pointers.** The helpers bridge mocked install calls and test stimulus.

They are split into two modules:

### `mock_usb_host_layer_callbacks.c` — callback capture

Registered on mocked `usbh_install()` / `enum_install()` / `hub_install()` and matching uninstall paths. Stores the real callbacks that `usb_host.c` passes in:

| Function                          | CMock registration                | Captures from `usb_host.c`                                                |
| --------------------------------- | --------------------------------- | ------------------------------------------------------------------------- |
| `usbh_register_mock_callback()`   | `usbh_install_AddCallback(...)`   | `usbh_event_callback`, `proc_req_callback`                                |
| `usbh_deregister_mock_callback()` | `usbh_uninstall_AddCallback(...)` | Clears USBH callbacks                                                     |
| `enum_register_mock_callback()`   | `enum_install_AddCallback(...)`   | `enum_event_callback`, `proc_req_callback`, `enum_filter_cb` (if enabled) |
| `enum_deregister_mock_callback()` | `enum_uninstall_AddCallback(...)` | Clears Enum callbacks                                                     |
| `hub_register_mock_callback()`    | `hub_install_AddCallback(...)`    | `hub_event_callback`, `proc_req_callback`                                 |
| `hub_deregister_mock_callback()`  | `hub_uninstall_AddCallback(...)`  | Clears Hub callbacks                                                      |

When `CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK` is enabled:

| Function                                   | Purpose                                                                              |
| ------------------------------------------ | ------------------------------------------------------------------------------------ |
| `mock_usb_host_layer_get_enum_filter_cb()` | Returns the filter callback forwarded from `usb_host_install()` via `enum_install()` |

### `mock_usb_host_layer_events.c` — event and processing-request injection

After install, tests call injectors to simulate what the lower layers would normally invoke on the Host layer.

**USBH events** (require `usbh_register_mock_callback()` first):

| Injector                        | Host-layer handler effect                                             |
| ------------------------------- | --------------------------------------------------------------------- |
| `usbh_mock_event_ctrl_xfer()`   | Completes a control transfer for the submitting client                |
| `usbh_mock_event_new_dev()`     | Client `NEW_DEV` broadcast (unless `hub_dev_new()` succeeds)          |
| `usbh_mock_event_dev_gone()`    | Client `DEV_GONE` to device owner; optional `DEV_REMOVED` to monitors |
| `usbh_mock_event_dev_free()`    | Calls `hub_node_recycle()`                                            |
| `usbh_mock_event_all_free()`    | Sets `USB_HOST_LIB_EVENT_FLAGS_ALL_FREE`                              |
| `usbh_mock_event_dev_suspend()` | Client `DEV_SUSPENDED` to device owner                                |
| `usbh_mock_event_dev_resume()`  | Client `DEV_RESUMED` to device owner                                  |
| `usbh_mock_event_all_idle()`    | Calls `hub_root_mark_suspend()`                                       |
| `usbh_mock_event_dev_removed()` | Client `DEV_REMOVED` to subscribed clients                            |

**Hub events** (require `hub_register_mock_callback()` first):

| Injector                           | Host-layer handler effect                      |
| ---------------------------------- | ---------------------------------------------- |
| `hub_mock_event_connected()`       | Calls `enum_start()`                           |
| `hub_mock_event_reset_completed()` | Calls `enum_proceed()`                         |
| `hub_mock_event_disconnected()`    | Calls `enum_cancel()` and `usbh_devs_remove()` |

**Enum events** (require `enum_register_mock_callback()` first):

| Injector                           | Host-layer handler effect                                 |
| ---------------------------------- | --------------------------------------------------------- |
| `enum_mock_event_started()`        | No-op in handler (enumeration started)                    |
| `enum_mock_event_reset_required()` | Calls `hub_node_reset()`                                  |
| `enum_mock_event_completed()`      | Calls `hub_node_active()` and `usbh_devs_new_dev_event()` |
| `enum_mock_event_canceled()`       | Calls `hub_node_disable()`                                |

**Processing requests** (wake `usb_host_lib_handle_events()`):

| Injector               | Triggers in `usb_host_lib_handle_events()` |
| ---------------------- | ------------------------------------------ |
| `usbh_mock_proc_req()` | `usbh_process()`                           |
| `enum_mock_proc_req()` | `enum_process()`                           |
| `hub_mock_proc_req()`  | `hub_process()`                            |

All injectors return `ESP_ERR_INVALID_STATE` if the relevant callback was not captured (typically: Host not installed, or install expectations missing the `AddCallback` hooks).

## How to use in a test

Include the umbrella header (or both public headers separately):

```cpp
#include "mock_usb_host_layer_test_helpers.h"
```

### Recommended: `UsbHostLayerFixture`

Most event tests use the shared fixture in `usb_host_layer_test_fixture.hpp` instead of hand-written install/uninstall expectations. It wraps `usb_host_install()` / `usb_host_uninstall()` and only registers the layer callbacks a test needs:

```cpp
#include "usb_host_layer_test_fixture.hpp"

GIVEN("An installed USB Host library with USBH callbacks registered") {
    usb_host_layer_test::UsbHostLayerFixture host(usb_host_layer_test::MOCK_CB_USBH);
    host.install();

    // ... test sections using usbh_mock_event_*() injectors ...

    usb_host_layer_test::drain_lib_events();
}
```

Callback flags:

| Flag           | Registers                            |
| -------------- | ------------------------------------ |
| `MOCK_CB_NONE` | No mock layer callbacks              |
| `MOCK_CB_USBH` | USBH injectors                       |
| `MOCK_CB_ENUM` | Enum injectors                       |
| `MOCK_CB_HUB`  | Hub injectors                        |
| `MOCK_CB_ALL`  | All of the above (combine with `\|`) |

Pass a custom `usb_host_config_t` as the second constructor argument to test `skip_phy_setup`, `root_port_unpowered`, multi-port `peripheral_map`, or `enum_filter_cb`.

### Manual install wiring (without fixture)

```cpp
extern "C" {
#include "Mockusb_phy.h"
#include "Mockusbh.h"
#include "Mockenum.h"
#include "Mockhub.h"
}

usb_new_phy_ExpectAnyArgsAndReturn(ESP_OK);
usb_new_phy_ReturnThruPtr_handle_ret(&phy_handle);
usbh_install_ExpectAnyArgsAndReturn(ESP_OK);
usbh_install_AddCallback(usbh_register_mock_callback);
enum_install_ExpectAnyArgsAndReturn(ESP_OK);
enum_install_AddCallback(enum_register_mock_callback);
hub_install_ExpectAnyArgsAndReturn(ESP_OK);
hub_install_AddCallback(hub_register_mock_callback);
hub_root_start_ExpectAndReturn(ESP_OK);

REQUIRE(ESP_OK == usb_host_install(&host_config));
```

Omit `hub_root_start_ExpectAndReturn()` when `host_config.root_port_unpowered == true`. Omit PHY expectations when `host_config.skip_phy_setup == true`.

### Inject an event and drain handlers

```cpp
hub_dev_new_ExpectAndReturn(dev_addr, ESP_ERR_NOT_SUPPORTED);
REQUIRE(ESP_OK == usbh_mock_event_new_dev(dev_addr));

REQUIRE(ESP_OK == usb_host_client_handle_events(client_hdl, 0));
```

### Uninstall wiring

```cpp
hub_root_stop_ExpectAndReturn(ESP_OK);
hub_uninstall_ExpectAndReturn(ESP_OK);
hub_uninstall_AddCallback(hub_deregister_mock_callback);
enum_uninstall_ExpectAndReturn(ESP_OK);
enum_uninstall_AddCallback(enum_deregister_mock_callback);
usbh_uninstall_ExpectAndReturn(ESP_OK);
usbh_uninstall_AddCallback(usbh_deregister_mock_callback);
usb_del_phy_ExpectAnyArgsAndReturn(phy_handle, ESP_OK);

REQUIRE(ESP_OK == usb_host_uninstall());
```

### CMock expectations for side effects

Many injectors call into other mocked layers. Set expectations **before** calling the injector, for example:

```cpp
hub_node_recycle_ExpectAndReturn(dev_uid, ESP_OK);
REQUIRE(ESP_OK == usbh_mock_event_dev_free(dev_uid, parent_hdl, port_num));

hub_node_active_ExpectAndReturn(node_uid, ESP_OK);
usbh_devs_new_dev_event_ExpectAndReturn(dev_hdl, ESP_OK);
REQUIRE(ESP_OK == enum_mock_event_completed(node_uid, dev_hdl));
```

After processing-request injectors, call `usb_host_lib_handle_events()` and expect the corresponding `*_process()` mock.

## What you can test with these helpers

The helpers are intended for **Host-layer behaviour** driven by lower-layer callbacks, without running real USBH/Enum/Hub code.

| Area                 | Examples                                              | Reference tests                           |
| -------------------- | ----------------------------------------------------- | ----------------------------------------- |
| Install / uninstall  | Valid and invalid configs, dual port, PHY errors      | `usb_host_install_unit_test.cpp`          |
| Install config       | `root_port_unpowered`, `skip_phy_setup`, enum filter  | `usb_host_install_config_unit_test.cpp`   |
| USBH event routing   | All `USBH_EVENT_*` injectors                          | `usb_host_usbh_events_unit_test.cpp`      |
| Client event routing | Multi-client routing, `max_num_event_msg`, ownership  | `usb_host_client_events_unit_test.cpp`    |
| Library events       | `ALL_FREE`, `NO_CLIENTS`, `AUTO_SUSPEND`, proc-req    | `usb_host_lib_events_unit_test.cpp`       |
| Enumeration handling | Hub node reset/active/disable, new-device propagation | `usb_host_enum_events_unit_test.cpp`      |
| Hub handling         | Connect / reset / disconnect enumeration flow         | `usb_host_hub_events_unit_test.cpp`       |
| Descriptor parsing   | `usb_helpers` descriptor utilities                    | `usb_helpers_descriptor_parsing_test.cpp` |

**Out of scope for these helpers:** testing real USBH/Enum/Hub driver logic (use target tests or layer-specific host mocks such as [`usbh_layer_test`](../../host_test/usbh_layer_test/)).

## Build and run

Host-layer tests that use this mock:

```bash
cd host/usb/test/host_test/usb_host_layer_test
idf.py --preview set-target linux
idf.py build
./build/host_test_usb_host_layer.elf
```

Verbose Catch2 output (per-section results and assertion expansions):

```bash
./build/host_test_usb_host_layer.elf -v high -s -d yes
./build/host_test_usb_host_layer.elf --list-tests
./build/host_test_usb_host_layer.elf --help
```

Ruby is required for CMock code generation.

**Note:** `E (...) USB HOST:` log lines during a full test run are expected. They come from negative install/error-path tests in `usb_host_install_unit_test.cpp`, not from test failures.
