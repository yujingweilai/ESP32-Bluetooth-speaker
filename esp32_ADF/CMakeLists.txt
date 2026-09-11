# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

set(ENV{ADF_PATH} "D:/ruanjiangjia/ESPIDF/vib_esp_adf")
include($ENV{ADF_PATH}/CMakeLists.txt)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(vib_player)
add_compile_definitions(A2DP_STREAM_QUEUE_SIZE=20)

string(TIMESTAMP DATE "%Y%m%d_%H%M%S")
message(STATUS "Firmware timestamp: ${DATE}")

add_custom_command(
    TARGET app
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin ${CMAKE_BINARY_DIR}/../firmware/${CMAKE_PROJECT_NAME}_V${PROJECT_VER}_${DATE}_fota.gbl
    COMMENT "Copying binary with version number"
)

# add_custom_command(
#     OUTPUT ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}_${PROJECT_VER}.bin
#     COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}_${PROJECT_VER}.bin
#     DEPENDS ${CMAKE_PROJECT_NAME}.bin
# )

# add_custom_target(copy_bin ALL DEPENDS ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}_${PROJECT_VER}.bin)

# ���ӿ�������
# get_target_property(BINARY_FILE all BINARY_FILE)
# add_custom_command(TARGET all POST_BUILD
#   COMMAND ${CMAKE_COMMAND} -E copy
#   "${BINARY_FILE}" 
#   "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}_${VERSION}.bin"
#   COMMENT "Copying binary with version number"
# )
