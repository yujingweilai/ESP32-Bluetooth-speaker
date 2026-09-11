idf_component_register(
  SRCS "charger.c" "bq256xx_drv.c"
  INCLUDE_DIRS "."
  REQUIRES 
    esp_driver_i2c
    esp_driver_gpio
    freertos
)
