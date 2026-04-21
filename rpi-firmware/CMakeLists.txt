cmake_minimum_required(VERSION 3.20.0)

list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_LIST_DIR}/modules/pico_usbip_host)
list(APPEND DTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/modules/pico_usbip_host)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pico_usbip_host)

target_sources(app PRIVATE src/main.c)
