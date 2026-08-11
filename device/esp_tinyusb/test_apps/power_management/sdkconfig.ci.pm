# Enable Tinyusb power management
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_TINYUSB_PM=y

# Tinyusb Suspend/Resume events are registered internally in sdkconfig.defaults
