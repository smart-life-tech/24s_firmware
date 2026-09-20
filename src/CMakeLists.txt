# ESP-IDF component definition for the 24S firmware
idf_component_register(
    SRCS
        "main.c"
        "hardware_init.c"
        "ltc6811.c"
        "scp_recovery.c"
        "third_wire.c"
        "pwm_mode.c"
        "subsystems.c"
        "ina240.c"
        "black_box.c"
        "mqtt_telemetry.c"
        "wifi_manager.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_wifi
        mqtt
        nvs_flash
        esp_adc
        spi_flash
        json
        esp_timer
        freertos
)
