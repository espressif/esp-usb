/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <ostream>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "esp_bit_defs.h"
#include "hub.h"    // Real implementation of hub.h
#include "usbh.h"   // Real implementation of usbh.h

// Test all the mocked headers defined for this mock
extern "C" {
#include "Mockhcd.h"
#include "Mockusb_private.h"
}

/*
These test cases cover the global (all root ports) power management of the Hub driver in a dual host configuration.
The mock builds the USB component with SOC_USB_OTG_PERIPH_NUM set to 2, so the Hub driver has two root ports and both
of them can hold a device at the same time, which is not reproducible on a target with a single USB-OTG peripheral.

Instead of setting up CMock expectations, the HCD is replaced by a minimal fake below. The fake keeps the state of both
ports and records every port command, so the test cases can assert which root ports were actually commanded.
*/

namespace {

constexpr int NUM_ROOT_PORTS = 2;

struct PortCommand {
    int port_idx;
    hcd_port_cmd_t cmd;

    bool operator==(const PortCommand &other) const
    {
        return port_idx == other.port_idx && cmd == other.cmd;
    }
};

const char *cmd_name(hcd_port_cmd_t cmd)
{
    switch (cmd) {
    case HCD_PORT_CMD_POWER_ON:  return "POWER_ON";
    case HCD_PORT_CMD_POWER_OFF: return "POWER_OFF";
    case HCD_PORT_CMD_RESET:     return "RESET";
    case HCD_PORT_CMD_SUSPEND:   return "SUSPEND";
    case HCD_PORT_CMD_RESUME:    return "RESUME";
    case HCD_PORT_CMD_DISABLE:   return "DISABLE";
    default:                     return "OTHER";
    }
}

/** Make the recorded port commands readable in the Catch2 output */
std::ostream &operator<<(std::ostream &os, const PortCommand &port_command)
{
    return os << "port " << port_command.port_idx << ": " << cmd_name(port_command.cmd);
}

/** Minimal fake of the HCD, shared by all the stubs below */
struct {
    hcd_port_state_t state[NUM_ROOT_PORTS];
    hcd_port_event_t pending_event[NUM_ROOT_PORTS];
    hcd_port_callback_t cb[NUM_ROOT_PORTS];
    void *cb_arg[NUM_ROOT_PORTS];
    std::vector<PortCommand> commands;
    // Error injection, armed by fail_next_command()
    struct {
        bool armed;
        hcd_port_cmd_t cmd;
        esp_err_t err;
    } cmd_failure[NUM_ROOT_PORTS];
} s_hcd;

int s_num_dev_suspend_events;
int s_num_dev_resume_events;
unsigned int s_last_connected_uid;

hcd_port_handle_t port_hdl(int port_idx)
{
    // Non-null opaque handles, so that the Hub driver can distinguish the root ports
    return reinterpret_cast<hcd_port_handle_t>(static_cast<uintptr_t>(0xB0510000 + port_idx));
}

int port_idx(hcd_port_handle_t hdl)
{
    for (int i = 0; i < NUM_ROOT_PORTS; i++) {
        if (hdl == port_hdl(i)) {
            return i;
        }
    }
    FAIL("Unknown HCD port handle");
    return 0;
}

// ------------------------------------------------- HCD stubs ---------------------------------------------------

esp_err_t port_init_stub(int port_number, const hcd_port_config_t *port_config, hcd_port_handle_t *port_hdl_ret, int cmock_num_calls)
{
    s_hcd.cb[port_number] = port_config->callback;
    s_hcd.cb_arg[port_number] = port_config->callback_arg;
    *port_hdl_ret = port_hdl(port_number);
    return ESP_OK;
}

esp_err_t port_deinit_stub(hcd_port_handle_t hdl, int cmock_num_calls)
{
    return ESP_OK;
}

esp_err_t port_command_stub(hcd_port_handle_t hdl, hcd_port_cmd_t command, int cmock_num_calls)
{
    const int idx = port_idx(hdl);
    s_hcd.commands.push_back({idx, command});

    if (s_hcd.cmd_failure[idx].armed && s_hcd.cmd_failure[idx].cmd == command) {
        s_hcd.cmd_failure[idx].armed = false;
        // A rejected command leaves the port in its previous state
        return s_hcd.cmd_failure[idx].err;
    }

    switch (command) {
    case HCD_PORT_CMD_POWER_ON:  s_hcd.state[idx] = HCD_PORT_STATE_DISCONNECTED; break;
    case HCD_PORT_CMD_POWER_OFF: s_hcd.state[idx] = HCD_PORT_STATE_NOT_POWERED;  break;
    case HCD_PORT_CMD_RESET:     s_hcd.state[idx] = HCD_PORT_STATE_ENABLED;      break;
    case HCD_PORT_CMD_SUSPEND:   s_hcd.state[idx] = HCD_PORT_STATE_SUSPENDED;    break;
    case HCD_PORT_CMD_RESUME:    s_hcd.state[idx] = HCD_PORT_STATE_ENABLED;      break;
    case HCD_PORT_CMD_DISABLE:   s_hcd.state[idx] = HCD_PORT_STATE_DISABLED;     break;
    default: break;
    }
    return ESP_OK;
}

hcd_port_state_t port_get_state_stub(hcd_port_handle_t hdl, int cmock_num_calls)
{
    return s_hcd.state[port_idx(hdl)];
}

hcd_port_event_t port_handle_event_stub(hcd_port_handle_t hdl, int cmock_num_calls)
{
    const int idx = port_idx(hdl);
    const hcd_port_event_t event = s_hcd.pending_event[idx];
    s_hcd.pending_event[idx] = HCD_PORT_EVENT_NONE;
    return event;
}

esp_err_t port_get_speed_stub(hcd_port_handle_t hdl, usb_speed_t *speed, int cmock_num_calls)
{
    *speed = USB_SPEED_FULL;
    return ESP_OK;
}

esp_err_t port_check_all_pipes_idle_stub(hcd_port_handle_t hdl, int cmock_num_calls)
{
    return ESP_OK;
}

esp_err_t port_recover_stub(hcd_port_handle_t hdl, int cmock_num_calls)
{
    s_hcd.state[port_idx(hdl)] = HCD_PORT_STATE_NOT_POWERED;
    return ESP_OK;
}

void *port_get_context_stub(hcd_port_handle_t hdl, int cmock_num_calls)
{
    return s_hcd.cb_arg[port_idx(hdl)];
}

esp_err_t pipe_alloc_stub(hcd_port_handle_t hdl, const hcd_pipe_config_t *pipe_config, hcd_pipe_handle_t *pipe_hdl_ret, int cmock_num_calls)
{
    // A non-null unique handle per root port is enough, the pipe itself is never used
    *pipe_hdl_ret = reinterpret_cast<hcd_pipe_handle_t>(static_cast<uintptr_t>(0xB1BE0000 + port_idx(hdl)));
    return ESP_OK;
}

esp_err_t pipe_free_stub(hcd_pipe_handle_t hdl, int cmock_num_calls)
{
    return ESP_OK;
}

esp_err_t pipe_command_stub(hcd_pipe_handle_t hdl, hcd_pipe_cmd_t command, int cmock_num_calls)
{
    return ESP_OK;
}

esp_err_t pipe_update_mps_stub(hcd_pipe_handle_t hdl, int mps, int cmock_num_calls)
{
    return ESP_OK;
}

// ------------------------------------------- Upper layer callbacks ---------------------------------------------

bool proc_req_cb(usb_proc_req_source_t source, bool in_isr, void *context)
{
    // The test drives hub_process() and usbh_process() explicitly, there is no host library task here
    return false;
}

void hub_event_cb(hub_event_data_t *event_data, void *arg)
{
    // Mimic the USB Host library, minus the enumeration which is driven by the test cases themselves
    switch (event_data->event) {
    case HUB_EVENT_CONNECTED:
        s_last_connected_uid = event_data->connected.uid;
        break;
    case HUB_EVENT_DISCONNECTED:
        usbh_devs_remove(event_data->disconnected.uid);
        break;
    default:
        break;
    }
}

void usbh_event_cb(usbh_event_data_t *event_data, void *arg)
{
    switch (event_data->event) {
    case USBH_EVENT_DEV_SUSPEND:
        s_num_dev_suspend_events++;
        break;
    case USBH_EVENT_DEV_RESUME:
        s_num_dev_resume_events++;
        break;
    case USBH_EVENT_DEV_FREE:
        // Mimic the USB Host library, which lets the Hub driver recycle the device's port
        hub_node_recycle(event_data->dev_free_data.dev_uid);
        break;
    default:
        break;
    }
}

// ------------------------------------------------ Test helpers -------------------------------------------------

/**
 * @brief Install USBH and the Hub driver with the given root ports enabled and powered ON
 */
void install_hub(unsigned port_map)
{
    s_hcd = {};
    s_num_dev_suspend_events = 0;
    s_num_dev_resume_events = 0;
    s_last_connected_uid = 0;

    hcd_port_init_Stub(port_init_stub);
    hcd_port_deinit_Stub(port_deinit_stub);
    hcd_port_command_Stub(port_command_stub);
    hcd_port_get_state_Stub(port_get_state_stub);
    hcd_port_handle_event_Stub(port_handle_event_stub);
    hcd_port_get_speed_Stub(port_get_speed_stub);
    hcd_port_check_all_pipes_idle_Stub(port_check_all_pipes_idle_stub);
    hcd_port_recover_Stub(port_recover_stub);
    hcd_port_get_context_Stub(port_get_context_stub);
    hcd_pipe_alloc_Stub(pipe_alloc_stub);
    hcd_pipe_free_Stub(pipe_free_stub);
    hcd_pipe_command_Stub(pipe_command_stub);
    hcd_pipe_update_mps_Stub(pipe_update_mps_stub);

    usbh_config_t usbh_config = {
        .proc_req_cb = proc_req_cb,
        .proc_req_cb_arg = nullptr,
        .event_cb = usbh_event_cb,
        .event_cb_arg = nullptr,
    };
    REQUIRE(ESP_OK == usbh_install(&usbh_config));

    hub_config_t hub_config = {
        .port_map = port_map,
        .proc_req_cb = proc_req_cb,
        .proc_req_cb_arg = nullptr,
        .event_cb = hub_event_cb,
        .event_cb_arg = nullptr,
        .intr_flags = 0,
        .fifo_config = nullptr,
    };
    void *client_hdl;
    REQUIRE(ESP_OK == hub_install(&hub_config, &client_hdl));
    REQUIRE(ESP_OK == hub_root_start());
    s_hcd.commands.clear();
}

/**
 * @brief Inject an HCD port event and let the Hub driver and USBH handle it
 */
void port_event(int idx, hcd_port_event_t event)
{
    s_hcd.pending_event[idx] = event;
    // The HCD calls the root port callback from an ISR, the Hub driver only defers the event to hub_process()
    s_hcd.cb[idx](port_hdl(idx), event, s_hcd.cb_arg[idx], true);
    REQUIRE(ESP_OK == hub_process());
    REQUIRE(ESP_OK == usbh_process());
}

/**
 * @brief Make the next occurrence of a port command on a root port fail
 *
 * @note A real HCD port rejects a command when its state machine changed underneath the command, which happens when
 *       the device disconnects exactly while the command is being issued. Such a race is hardly reproducible on real
 *       hardware, especially with two root ports involved.
 */
void fail_next_command(int idx, hcd_port_cmd_t cmd, esp_err_t err)
{
    s_hcd.cmd_failure[idx] = {true, cmd, err};
}

/**
 * @brief Bring a root port to the enabled state by connecting a device to it
 */
void connect_device(int idx)
{
    port_event(idx, HCD_PORT_EVENT_CONNECTION);
}

void uninstall_hub(unsigned port_map)
{
    for (int i = 0; i < NUM_ROOT_PORTS; i++) {
        if (port_map & BIT(i)) {
            port_event(i, HCD_PORT_EVENT_DISCONNECTION);
        }
    }
    // The recycle path spans both processing loops: device free -> node recycle -> port disable
    REQUIRE(ESP_OK == hub_process());

    REQUIRE(ESP_OK == hub_root_stop());
    REQUIRE(ESP_OK == hub_uninstall());
    REQUIRE(ESP_OK == usbh_uninstall());
}

} // namespace

SCENARIO("Hub root PM: dual host global suspend and resume")
{
    GIVEN("Both root ports enabled with a connected device") {
        install_hub(BIT0 | BIT1);
        connect_device(0);
        connect_device(1);
        s_hcd.commands.clear();

        WHEN("The Host is suspended") {
            REQUIRE(ESP_OK == hub_root_can_suspend());
            REQUIRE(ESP_OK == hub_root_mark_suspend());
            REQUIRE(ESP_OK == hub_process());

            THEN("Both root ports are commanded to suspend") {
                const std::vector<PortCommand> expected = {
                    {0, HCD_PORT_CMD_SUSPEND},
                    {1, HCD_PORT_CMD_SUSPEND},
                };
                REQUIRE(s_hcd.commands == expected);
                REQUIRE(hub_root_is_suspended());
            }

            THEN("Each device is suspended exactly once") {
                REQUIRE(ESP_OK == usbh_process());
                REQUIRE(s_num_dev_suspend_events == 2);
            }

            THEN("Both root ports are commanded to resume") {
                s_hcd.commands.clear();
                REQUIRE(ESP_OK == hub_root_can_resume());
                REQUIRE(ESP_OK == hub_root_mark_resume());
                REQUIRE(ESP_OK == hub_process());

                const std::vector<PortCommand> expected = {
                    {0, HCD_PORT_CMD_RESUME},
                    {1, HCD_PORT_CMD_RESUME},
                };
                REQUIRE(s_hcd.commands == expected);
                REQUIRE_FALSE(hub_root_is_suspended());

                // Each device is resumed exactly once
                REQUIRE(ESP_OK == usbh_process());
                REQUIRE(s_num_dev_resume_events == 2);
            }

            THEN("A remote wakeup on one root port resumes both of them") {
                REQUIRE(ESP_OK == usbh_process());
                s_hcd.commands.clear();

                // The device behind the second root port signals resume upstream
                port_event(1, HCD_PORT_EVENT_REMOTE_WAKEUP);

                // The remote wakeup makes the whole Host active, not just the root port that reported it
                const std::vector<PortCommand> expected = {
                    {0, HCD_PORT_CMD_RESUME},
                    {1, HCD_PORT_CMD_RESUME},
                };
                REQUIRE(s_hcd.commands == expected);
                REQUIRE_FALSE(hub_root_is_suspended());

                // Each device is resumed exactly once
                REQUIRE(s_num_dev_resume_events == 2);
            }
        }

        uninstall_hub(BIT0 | BIT1);
    }
}

SCENARIO("Hub root PM: root port without a device is not suspended")
{
    GIVEN("Both root ports enabled, a device connected to the second one only") {
        install_hub(BIT0 | BIT1);
        connect_device(1);
        s_hcd.commands.clear();

        WHEN("The Host is suspended") {
            REQUIRE(ESP_OK == hub_root_can_suspend());
            REQUIRE(ESP_OK == hub_root_mark_suspend());
            REQUIRE(ESP_OK == hub_process());

            THEN("Only the root port with a device is commanded to suspend") {
                const std::vector<PortCommand> expected = {{1, HCD_PORT_CMD_SUSPEND}};
                REQUIRE(s_hcd.commands == expected);

                // The empty root port does not prevent the Host from being reported as suspended
                REQUIRE(hub_root_is_suspended());
            }
        }

        uninstall_hub(BIT0 | BIT1);
    }
}

SCENARIO("Hub root PM: no root port has a device")
{
    GIVEN("Both root ports enabled without a connected device") {
        install_hub(BIT0 | BIT1);

        THEN("The Host can neither be suspended nor resumed") {
            REQUIRE(ESP_ERR_NOT_ALLOWED == hub_root_can_suspend());
            REQUIRE(ESP_ERR_NOT_ALLOWED == hub_root_mark_suspend());
            REQUIRE(ESP_ERR_NOT_ALLOWED == hub_root_can_resume());
            REQUIRE(ESP_ERR_NOT_ALLOWED == hub_root_mark_resume());
            REQUIRE_FALSE(hub_root_is_suspended());
            REQUIRE(s_hcd.commands.empty());
        }

        uninstall_hub(BIT0 | BIT1);
    }
}

/*
@todo define reaction to these errors
SCENARIO("Hub root PM: one root port fails to suspend")
{
    GIVEN("Both root ports enabled with a connected device") {
        install_hub(BIT0 | BIT1);
        connect_device(0);
        connect_device(1);
        s_hcd.commands.clear();

        WHEN("The first root port rejects the suspend command") {
            fail_next_command(0, HCD_PORT_CMD_SUSPEND, ESP_ERR_INVALID_RESPONSE);
            REQUIRE(ESP_OK == hub_root_mark_suspend());
            REQUIRE(ESP_OK == hub_process());

            THEN("The other root port is suspended anyway") {
                const std::vector<PortCommand> expected = {
                    {0, HCD_PORT_CMD_SUSPEND},
                    {1, HCD_PORT_CMD_SUSPEND},
                };
                REQUIRE(s_hcd.commands == expected);

                // The failed root port keeps sending SOFs, so the Host as a whole is not suspended
                REQUIRE_FALSE(hub_root_is_suspended());
            }

            THEN("Each device is still suspended exactly once") {
                REQUIRE(ESP_OK == usbh_process());

                // The client facing PM state is global, so the root port that did suspend propagates the event to every
                // device. The device behind the failed root port is disconnecting (that is why the command was rejected),
                // so it does not need a PM state of its own.
                REQUIRE(s_num_dev_suspend_events == 2);
            }

            THEN("The failed root port can be suspended by a new attempt") {
                s_hcd.commands.clear();
                REQUIRE(ESP_OK == hub_root_can_suspend());
                REQUIRE(ESP_OK == hub_root_mark_suspend());
                REQUIRE(ESP_OK == hub_process());

                // Only the root port that is still enabled is commanded again
                const std::vector<PortCommand> expected = {{0, HCD_PORT_CMD_SUSPEND}};
                REQUIRE(s_hcd.commands == expected);
                REQUIRE(hub_root_is_suspended());
            }
        }

        uninstall_hub(BIT0 | BIT1);
    }
}

SCENARIO("Hub root PM: one root port fails to resume")
{
    GIVEN("Both root ports suspended with a connected device") {
        install_hub(BIT0 | BIT1);
        connect_device(0);
        connect_device(1);
        REQUIRE(ESP_OK == hub_root_mark_suspend());
        REQUIRE(ESP_OK == hub_process());
        REQUIRE(ESP_OK == usbh_process());
        REQUIRE(hub_root_is_suspended());
        s_hcd.commands.clear();

        WHEN("The second root port rejects the resume command") {
            fail_next_command(1, HCD_PORT_CMD_RESUME, ESP_ERR_INVALID_STATE);
            REQUIRE(ESP_OK == hub_root_mark_resume());
            REQUIRE(ESP_OK == hub_process());

            THEN("The other root port is resumed anyway") {
                const std::vector<PortCommand> expected = {
                    {0, HCD_PORT_CMD_RESUME},
                    {1, HCD_PORT_CMD_RESUME},
                };
                REQUIRE(s_hcd.commands == expected);

                // One active root port is enough to make the Host active
                REQUIRE_FALSE(hub_root_is_suspended());
            }

            THEN("Each device is still resumed exactly once") {
                REQUIRE(ESP_OK == usbh_process());
                REQUIRE(s_num_dev_resume_events == 2);
            }

            THEN("The failed root port can be resumed by a new attempt") {
                s_hcd.commands.clear();
                REQUIRE(ESP_OK == hub_root_can_resume());
                REQUIRE(ESP_OK == hub_root_mark_resume());
                REQUIRE(ESP_OK == hub_process());

                // Only the root port that is still suspended is commanded again
                const std::vector<PortCommand> expected = {{1, HCD_PORT_CMD_RESUME}};
                REQUIRE(s_hcd.commands == expected);
                REQUIRE_FALSE(hub_root_is_suspended());
            }
        }

        uninstall_hub(BIT0 | BIT1);
    }
}
*/

SCENARIO("Hub root PM: attach on one root port resumes the other one")
{
    GIVEN("A suspended root port and an empty root port") {
        install_hub(BIT0 | BIT1);
        connect_device(0);
        REQUIRE(ESP_OK == hub_root_mark_suspend());
        REQUIRE(ESP_OK == hub_process());
        REQUIRE(hub_root_is_suspended());
        s_hcd.commands.clear();

        WHEN("A device is attached to the empty root port") {
            connect_device(1);

            THEN("The suspended root port is resumed together with the attach") {
                // The PM state of the root ports must stay symmetric
                const std::vector<PortCommand> expected = {
                    {1, HCD_PORT_CMD_RESET},
                    {0, HCD_PORT_CMD_RESUME},
                };
                REQUIRE(s_hcd.commands == expected);
                REQUIRE_FALSE(hub_root_is_suspended());
            }

            THEN("Only the previously suspended device is resumed") {
                REQUIRE(ESP_OK == usbh_process());
                // The freshly attached device was never suspended, so it must not get a resumed event either
                REQUIRE(s_num_dev_resume_events == 1);
            }

            THEN("The freshly attached device can still be enumerated") {
                /*
                Mimic the beginning of the enumeration of the new device. The global resume must leave the device in
                the DEFAULT state, otherwise the enumeration driver cannot set its EP0 MPS and the device never
                enumerates.
                */
                usb_device_handle_t dev_hdl;
                REQUIRE(ESP_OK == usbh_devs_open_uid(s_last_connected_uid, &dev_hdl));
                REQUIRE(ESP_OK == usbh_dev_enum_lock(dev_hdl));
                REQUIRE(ESP_OK == usbh_dev_set_ep0_mps(dev_hdl, 64));
                REQUIRE(ESP_OK == usbh_dev_enum_unlock(dev_hdl));
                REQUIRE(ESP_OK == usbh_dev_close(dev_hdl));
            }
        }

        uninstall_hub(BIT0 | BIT1);
    }
}
