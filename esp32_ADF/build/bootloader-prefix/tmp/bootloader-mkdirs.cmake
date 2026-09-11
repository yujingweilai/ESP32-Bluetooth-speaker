# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "D:/ruanjiangjia/ESPIDF/v551/Espressif/frameworks/esp-idf-v5.5.1/components/bootloader/subproject")
  file(MAKE_DIRECTORY "D:/ruanjiangjia/ESPIDF/v551/Espressif/frameworks/esp-idf-v5.5.1/components/bootloader/subproject")
endif()
file(MAKE_DIRECTORY
  "D:/code/VIB/new_git/jack-vib_player/build/bootloader"
  "D:/code/VIB/new_git/jack-vib_player/build/bootloader-prefix"
  "D:/code/VIB/new_git/jack-vib_player/build/bootloader-prefix/tmp"
  "D:/code/VIB/new_git/jack-vib_player/build/bootloader-prefix/src/bootloader-stamp"
  "D:/code/VIB/new_git/jack-vib_player/build/bootloader-prefix/src"
  "D:/code/VIB/new_git/jack-vib_player/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "D:/code/VIB/new_git/jack-vib_player/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "D:/code/VIB/new_git/jack-vib_player/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
