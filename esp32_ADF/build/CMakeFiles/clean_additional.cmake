# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "")
  file(REMOVE_RECURSE
  "bloop_1.mp3.S"
  "bloop_2.mp3.S"
  "bootloader\\bootloader.bin"
  "bootloader\\bootloader.elf"
  "bootloader\\bootloader.map"
  "config\\sdkconfig.cmake"
  "config\\sdkconfig.h"
  "esp-idf\\esptool_py\\flasher_args.json.in"
  "esp-idf\\mbedtls\\x509_crt_bundle"
  "flash_app_args"
  "flash_bootloader_args"
  "flash_project_args"
  "flasher_args.json"
  "greanpatch.mp3.S"
  "ldgen_libraries"
  "ldgen_libraries.in"
  "mixkit_click_error.mp3.S"
  "project_elf_src_esp32.c"
  "shooting.mp3.S"
  "vib_player.bin"
  "vib_player.map"
  "x509_crt_bundle.S"
  )
endif()
