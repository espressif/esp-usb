idf_component_register(SRCS
                        "cdc_acm_host.c"                # Contains driver and transfers code
                        "cdc_host_descriptor_parsing.c" # Only descriptor parsing code, no CDC device handling
                        "cdc_host_acm_compliant.c"      # Implementation of CDC ACM compliant functions
                        "cdc_host_ops.c"                # Implementation of CDC ACM host operations
                       INCLUDE_DIRS "include" "interface"
                       PRIV_INCLUDE_DIRS "private_include" "include/esp_private"
                       REQUIRES usb
                       )
