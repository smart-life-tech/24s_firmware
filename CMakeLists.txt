# CMakeLists.txt — top level
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(24s_smart_hub)

---
# main/CMakeLists.txt
idf_component_register(
    SRCS
        "main.c"
        "hardware_init.c"
        "ltc6813.c"
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
        esp_adc_cal
        spi_flash
        cJSON
        esp_timer
        freertos
)
