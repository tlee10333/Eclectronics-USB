# SPDX-License-Identifier: Apache-2.0

zephyr_library()

set(PICO_USBIP_USE_PIO_BACKEND OFF)
set(PICO_USBIP_USE_NATIVE_BACKEND OFF)

if(CONFIG_UHC_RP2350_PIO_USB)
  set(PICO_USBIP_USE_PIO_BACKEND ON)
  set(PIO_USB_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/../../modules/pico_pio_usb/src)

  zephyr_include_directories(
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${PIO_USB_SRC_DIR}
    ${ZEPHYR_BASE}/drivers/usb/uhc
  )

  zephyr_library_compile_options(
    -include
    ${CMAKE_CURRENT_LIST_DIR}/include/pio_usb_zephyr_shim.h
    -O3
  )

  zephyr_library_sources(
    ${PIO_USB_SRC_DIR}/pio_usb.c
    ${PIO_USB_SRC_DIR}/pio_usb_host.c
    ${PIO_USB_SRC_DIR}/usb_crc.c
    src/pio_usb_host_task_stub.c
    src/uhc_rp2350_pio_usb.c
  )
endif()

if(CONFIG_UHC_RP2_NATIVE_USB)
  set(PICO_USBIP_USE_NATIVE_BACKEND ON)
endif()

if(PICO_USBIP_USE_NATIVE_BACKEND OR (NOT PICO_USBIP_USE_PIO_BACKEND AND NOT PICO_USBIP_USE_NATIVE_BACKEND))
  if(NOT PICO_USBIP_USE_NATIVE_BACKEND)
    message(STATUS
      "Neither CONFIG_UHC_RP2350_PIO_USB nor CONFIG_UHC_RP2_NATIVE_USB is enabled; "
      "defaulting pico_usbip_host to native RP2 backend sources.")
  endif()
  set(PICO_SDK_DIR ${CMAKE_CURRENT_LIST_DIR}/../../modules/pico-sdk)
  set(TINYUSB_SRC_DIR ${PICO_SDK_DIR}/lib/tinyusb/src)
  set(PICO_SDK_ZEPHYR_HAL_DIR ${ZEPHYR_HAL_RPI_PICO_MODULE_DIR}/src)
  set(PICO_SDK_COMMON_DIR ${PICO_SDK_ZEPHYR_HAL_DIR}/common)
  set(PICO_SDK_RP2_COMMON_DIR ${PICO_SDK_ZEPHYR_HAL_DIR}/rp2_common)

  zephyr_include_directories(
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${ZEPHYR_BASE}/drivers/usb/uhc
    ${PICO_SDK_COMMON_DIR}/pico_base_headers/include
    ${PICO_SDK_COMMON_DIR}/pico_platform_compiler/include
    ${PICO_SDK_COMMON_DIR}/pico_platform_sections/include
    ${PICO_SDK_COMMON_DIR}/pico_sync/include
    ${PICO_SDK_COMMON_DIR}/pico_time/include
    ${PICO_SDK_RP2_COMMON_DIR}/hardware_base/include
    ${PICO_SDK_RP2_COMMON_DIR}/hardware_structs/include
    ${PICO_SDK_RP2_COMMON_DIR}/hardware_regs/include
    ${PICO_SDK_RP2_COMMON_DIR}/hardware_irq/include
    ${PICO_SDK_RP2_COMMON_DIR}/hardware_resets/include
    ${PICO_SDK_RP2_COMMON_DIR}/hardware_timer/include
    ${PICO_SDK_RP2_COMMON_DIR}/pico_platform/include
    ${TINYUSB_SRC_DIR}
    ${TINYUSB_SRC_DIR}/common
    ${TINYUSB_SRC_DIR}/host
    ${TINYUSB_SRC_DIR}/portable/raspberrypi/rp2040
  )

  zephyr_library_compile_definitions(
    CFG_TUSB_MCU=OPT_MCU_RP2040
    CFG_TUSB_OS=OPT_OS_NONE
    CFG_TUH_ENABLED=1
    CFG_TUD_ENABLED=0
    CFG_TUH_RPI_PIO_USB=0
    CFG_TUH_MAX3421=0
  )

  zephyr_library_sources(
    ${TINYUSB_SRC_DIR}/tusb.c
    ${TINYUSB_SRC_DIR}/common/tusb_fifo.c
    ${TINYUSB_SRC_DIR}/host/usbh.c
    ${TINYUSB_SRC_DIR}/host/hub.c
    ${TINYUSB_SRC_DIR}/portable/raspberrypi/rp2040/rp2040_usb.c
    ${TINYUSB_SRC_DIR}/portable/raspberrypi/rp2040/hcd_rp2040.c
    src/tinyusb_zephyr_port.c
    src/uhc_rp2_native_usb.c
  )
endif()
