# ESP Type-C (esp-usb/type_c)

> **Status:** Beta API - names and behavior may change before v1.0.

This component provides a minimal Type-C CC API for attach/detach, cable
orientation, and power-role control. The current implementation uses the
FUSB302 TCPC for CC detection and interrupt handling. This component only
implements basic Type-C attach/detach and role control.

______________________________________________________________________

## Contents

- `include/esp_typec.h` - public Type-C CC API
- `include_private/` - backend-only types
- `src/` - core logic and the FUSB302 backend
- `CMakeLists.txt`, `idf_component.yml` - component metadata

______________________________________________________________________

## Supported Controllers

| Controller | Capability | API / Factory Function            | Notes                             |
| ---------: | ---------- | --------------------------------- | --------------------------------- |
|    FUSB302 | Type-C CC  | `esp_typec_port_create_fusb302()` | CC attach/detach and role control |

______________________________________________________________________

## Quick Start

### Type-C CC with FUSB302

```c
#include "esp_event.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_typec.h"

static void on_typec_event(void *handler_arg,
                           esp_event_base_t base,
                           int32_t id,
                           void *event_data)
{
    (void)handler_arg;
    (void)base;

    switch (id) {
    case ESP_TYPEC_EVENT_ID_ATTACHED: {
        const esp_typec_evt_attached_t *a = (const esp_typec_evt_attached_t *)event_data;
        bool cc2 = a ? a->cc2_active : false;
        uint32_t ma = a ? a->rp_cur_ma : 0;
        ESP_LOGI("typec", "ATTACHED: cc2=%d rp=%u mA", cc2 ? 1 : 0, (unsigned)ma);
        break;
    }
    case ESP_TYPEC_EVENT_ID_DETACHED:
        ESP_LOGI("typec", "DETACHED");
        break;
    case ESP_TYPEC_EVENT_ID_ERROR:
    default:
        ESP_LOGW("typec", "EVENT %d", (int)id);
        break;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_typec_install(NULL));

    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = 7,
        .scl_io_num = 8,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    esp_typec_port_config_t port_cfg = {
        .default_power_role = ESP_TYPEC_PWR_SOURCE,
        .task_stack = 4096,
        .task_prio = 5,
        .rp_current = ESP_TYPEC_RP_3A0,
        .src_vbus_gpio = 28,
        .src_vbus_gpio_n = 15,
    };

    esp_typec_fusb302_config_t hw = {
        .i2c_bus = bus,
        .i2c_addr_7b = 0x22,
        .gpio_int = 9,
    };

    esp_typec_port_handle_t port = NULL;
    ESP_ERROR_CHECK(esp_typec_port_create_fusb302(&port_cfg, &hw, &port));

    ESP_ERROR_CHECK(esp_event_handler_register(
                        ESP_TYPEC_EVENT,
                        ESP_EVENT_ANY_ID,
                        &on_typec_event,
                        NULL));
}
```

______________________________________________________________________

## Notes

- Event IDs are `esp_typec_event_id_t` values posted on `ESP_TYPEC_EVENT`.
