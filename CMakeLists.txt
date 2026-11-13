cmake_minimum_required(VERSION 3.20.0)
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nrf54l15testapp LANGUAGES C)
target_include_directories(app PRIVATE include vendor/ad5940)
target_compile_definitions(app PRIVATE CHIPSEL_594X)
target_sources(app PRIVATE src/main.c src/board_io.c src/ad5941_port.c
  src/ad5941.c src/methods.c src/protocol.c src/ble_link.c vendor/ad5940/ad5940.c)

