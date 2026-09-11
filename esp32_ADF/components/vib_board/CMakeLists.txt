# Edit following two lines to set component requirements (see docs)
set(COMPONENT_REQUIRES esp_adc esp_driver_i2c esp_driver_gpio)
set(COMPONENT_PRIV_REQUIRES audio_sal audio_hal esp_dispatcher esp_peripherals display_service periph_key periph_battery charger oled_ssd1315)

if(CONFIG_AUDIO_BOARD_CUSTOM)

message(STATUS "Current board name is " CONFIG_AUDIO_BOARD_CUSTOM)
if (CONFIG_VIB_BOARD_V1_0)
message(STATUS "board version " CONFIG_VIB_BOARD_V1_0)
list(APPEND COMPONENT_ADD_INCLUDEDIRS ./board_v1_0 )
set(COMPONENT_SRCS
./board_v1_0/board.c
./board_v1_0/board_pins_config.c
)
endif()
if (CONFIG_VIB_BOARD_V3_0)
message(STATUS "board version " CONFIG_VIB_BOARD_V3_0)
list(APPEND COMPONENT_ADD_INCLUDEDIRS ./board_v3_0 )
set(COMPONENT_SRCS
./board_v3_0/board.c
./board_v3_0/board_pins_config.c
)
endif()

endif()

register_component()

IF (IDF_VERSION_MAJOR GREATER 3)
idf_component_get_property(audio_board_lib audio_board COMPONENT_LIB)
set_property(TARGET ${audio_board_lib} APPEND PROPERTY INTERFACE_LINK_LIBRARIES ${COMPONENT_LIB})

ELSEIF (IDF_VERSION_MAJOR EQUAL 3)
set_property(TARGET idf_component_audio_board APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES  $<TARGET_PROPERTY:${COMPONENT_TARGET},INTERFACE_INCLUDE_DIRECTORIES>)

ENDIF (IDF_VERSION_MAJOR GREATER 3)
