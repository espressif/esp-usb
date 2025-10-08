# ESP Type-C / USB Power Delivery (esp-usb/type_c)

> **Status:** Beta API — names and behavior may change before v1.0.

This component provides two focused, minimal public APIs for USB Type-C:

- **Type-C Core (CC-only)** — attach/detach, orientation (CC1/CC2), and power-role presence (Rp/Rd).
  Backed by **HUSB320** (Type-C CC controller; **no PD**).
- **USB Power Delivery (PD-only)** — PD negotiation, runtime power-role control, fixed/PPS sink requests, and contract reporting.
  Backed by **FUSB302** (PD-capable TCPC).

The split keeps builds small, avoids unnecessary coupling, and mirrors esp-usb / esp-idf style.

---

## Contents

- [`include/esp_typec.h`](./include/esp_typec.h) — **Type-C Core (CC-only)** public API
- [`include/esp_typec_pd.h`](./include/esp_typec_pd.h) — **PD-only** public API
- `src/` — stub sources to make the component build; real backends land incrementally
- `CMakeLists.txt`, `idf_component.yml` — component metadata

> New files use:
> `/* SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD */`
> `/* SPDX-License-Identifier: Apache-2.0 */`

---

## Supported Controllers

| Controller | Capability | API / Factory Function                                   | Notes                          |
|-----------:|------------|-----------------------------------------------------------|--------------------------------|
| HUSB320    | Type-C CC  | `esp_typec_port_create_husb320()` (in `esp_typec.h`)     | CC attach/orientation only     |
| FUSB302    | PD (TCPC)  | `esp_typec_pd_port_create_fusb302()` (in `esp_typec_pd.h`)| Full PD PHY over CC (BMC)      |

> **Not supported here:** CC-only parts for PD; they cannot negotiate PD by design.
> Additional PD-capable TCPCs (e.g., PTN5110/TCPM family) can be added later using the same factory pattern.

---

## Quick Start

### 1) Type-C Core (CC-only) with HUSB320

```c
#include "esp_typec.h"

static void on_typec(esp_typec_event_t evt, const void *payload, void *arg)
{
    switch (evt) {
    case ESP_TYPEC_EVENT_ATTACHED: {
        const esp_typec_evt_attached_t *a = payload;
        // a->flags & ESP_TYPEC_FLAG_CC2   -> polarity
        // a->rp_cur_ma                    -> advertised current (if known)
        break;
    }
    case ESP_TYPEC_EVENT_ORIENTATION: {
        const bool *cc2_active = payload;
        (void)cc2_active;
        break;
    }
    case ESP_TYPEC_EVENT_DETACHED:
    default: break;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_typec_install());

    esp_typec_port_config_t port_cfg = {
        .default_power_role = ESP_TYPEC_PWR_SINK,
        .try_snk = true,
        .try_src = false,
        .task_stack = 0,
        .task_prio = 0,
    };
    esp_typec_husb320_config_t hw = {
        .i2c_port = 0, .i2c_addr = 0x60, .gpio_int = 4, .use_intr = true,
    };
    esp_typec_port_handle_t h;
    ESP_ERROR_CHECK(esp_typec_port_create_husb320(&port_cfg, &hw, on_typec, NULL, &h));

    // Optional: request a role change (sets Rp/Rd)
    ESP_ERROR_CHECK(esp_typec_set_power_role(h, ESP_TYPEC_PWR_DRP));
}
```

### 1) Power Delivery (PD) with FUSB302

```c
#include "esp_typec_pd.h"

static void on_pd(esp_typec_pd_event_t evt, const void *payload, void *arg)
{
    switch (evt) {
    case ESP_TYPEC_PD_EVENT_CONTRACT_READY: {
        const esp_typec_pd_contract_t *c = payload;
        // c->mv, c->ma, c->selected_pdo, c->flags (PPS/CC2)
        break;
    }
    case ESP_TYPEC_PD_EVENT_ATTACHED:
    case ESP_TYPEC_PD_EVENT_DETACHED:
    default: break;
    }
}

void app_main(void)
{
    esp_typec_pd_install_config_t lib_cfg = {
        .intr_flags = 0,
    };
    ESP_ERROR_CHECK(esp_typec_pd_install(&lib_cfg));

    esp_typec_pd_port_config_t port_cfg = {
        .default_power_role   = ESP_TYPEC_PD_PWR_SINK,
        .sink_i_min_ma        = 500,
        .sink_fixed_pref_mv   = 9000,    // 0 = auto select best
        .enable_pps           = true,
        .sink_pps_v_min_mv    = 5000, .sink_pps_v_max_mv = 11000, .sink_pps_i_max_ma = 2000,
        .src_pdos             = NULL, .src_pdo_count = 0,
        .task_stack           = 0, .task_prio = 0,
    };
    esp_typec_pd_fusb302_config_t hw = {
        .i2c_port = 0, .i2c_addr = 0x22, .gpio_int = 5, .use_intr = true,
    };
    esp_typec_pd_port_handle_t h;
    ESP_ERROR_CHECK(esp_typec_pd_port_create_fusb302(&port_cfg, &hw, on_pd, NULL, &h));

    // Example: request a specific Fixed PDO after Source_Capabilities are received
    // ESP_ERROR_CHECK(esp_typec_pd_sink_request_fixed(h, 9000, 2000));
}
```
