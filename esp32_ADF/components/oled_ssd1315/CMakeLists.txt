idf_component_register(
    SRCS 
        "oled_ssd1315.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_i2c
        esp_driver_gpio
        freertos
)