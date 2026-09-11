include(${PROJECT_DIR}/main/tone/embed_tone.cmake)

set(COMPONENT_REQUIRES
    app_update
    audio_stream
    bluetooth_service
    bootloader_support
    bt
    esp_partition
    input_key_service
    playlist
    spiffs
    vib_board
    periph_key
    periph_battery
    wifi_service
)

set(COMPONENT_SRCS
    "lib/fw_timer.c"
    "app_player_sdcard.c"
    "app_gatt_uart.c"
    "app_bt.c"
    "app_player.c"
    "app_control.c"
    "app_sys_tools.c"
    "main.c"
    "audio_embed_tone.c"
    "app_ble.c"
    "app_gatt_ota.c"
    "app_gatt_device.c"
)

set(COMPONENT_ADD_INCLUDEDIRS "." "lib")

register_component()
