# Explicitly selecting USB Host component

If USB component is set as dependency in a `idf_component.yml` file, the component manager chooses the `usb` component automatically

```yml
## IDF Component Manager Manifest File
dependencies:
  espressif/usb:
    version: "*"
```

A test application, or an example can be explicitly built with:

- a managed USB host component from [esp-usb](../../../host/usb/) (this repository)
- a native USB component included in [esp-idf](https://github.com/espressif/esp-idf/tree/release/v5.5/components/usb)

In some test apps and examples, within this repository, the selection is done manually to support CI builds with both USB components.

An example of a `idf_component.yml` file with manual `usb` component selection:

```yml
## IDF Component Manager Manifest File
dependencies:
  espressif/usb:
    version: "*"
    override_path: "../../../../../usb"
    rules:
      - if: "$ENV_VAR_USB_COMP_MANAGED == yes"  # Environmental variable to select between managed (esp-usb) and native (esp-idf) USB Component
```

The component selection is done by the environment variable `ENV_VAR_USB_COMP_MANAGED`, which must be explicitly set to `yes` or `no` before building. If the variable is not exported, the build will fail with an error similar to:

```bash
ERROR: Environment variable "ENV_VAR_USB_COMP_MANAGED" is not set
```

Note that the managed USB component can only be used with service [ESP-IDF](https://github.com/espressif/esp-idf?tab=readme-ov-file#esp-idf-release-support-schedule) releases

| `ENV_VAR_USB_COMP_MANAGED` | USB Component used      | Note                        |
| -------------------------- | ----------------------- | --------------------------- |
| `yes`                      | managed `USB` component | Requires `IDF 5.4` or later |
| `no`                       | native `USB` component  | Requires `IDF 5.x`          |

### Exporting the environmental variable

```bash
# Linux/macOS (lasts only in the current terminal session)
export ENV_VAR_USB_COMP_MANAGED=yes
idf.py set-target esp32s3 build

# Windows (PowerShell, lasts only in the current terminal session)
set ENV_VAR_USB_COMP_MANAGED=yes
idf.py set-target esp32s3 build

# Windows (PowerShell, persistent across sessions)
setx ENV_VAR_USB_COMP_MANAGED yes

```
