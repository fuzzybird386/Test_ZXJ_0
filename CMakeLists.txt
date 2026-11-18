cmake_minimum_required(VERSION 3.20.0)
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ecw_single_channel)
target_sources(app PRIVATE
 src/main.c src/power.c src/led.c src/ble.c src/ble_conn.c src/ble_bat.c
 src/ble_aabb.c src/ble_aabb_aab0_ctrl.c src/ble_aabb_aabf_data.c src/ble_aabb_aabe_log.c
 src/method/ec_method.c src/method/methods.c src/method/method_cv.c src/method/method_dpv.c
 src/method/proxy/ec_proxy_bus.c src/method/proxy/ec_proxy_lpamp.c
 src/method/proxy/ec_proxy_cv.c src/method/proxy/ec_proxy_dpv.c
 src/mux/mux.c src/ad5941_port/ad5940_port.c driver/battery.c lib/ad5940/ad5940.c)
target_include_directories(app PRIVATE . src src/method src/ad5941_port lib/ad5940)
target_compile_definitions(app PRIVATE CHIPSEL_594X=1 ECW_AUTOTEST_ENABLE=0)
