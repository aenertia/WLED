cmake_minimum_required(VERSION 3.13)

# Pull in Pico SDK (set PICO_SDK_PATH or let FetchContent grab it)
include(pico_sdk_import.cmake)

project(pico_ncm_ppp_bridge C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pico_bridge
    main.c
    usb_descriptors.c
    dhcp_server.c
    nat_relay.c
)

# Our headers (tusb_config.h, lwipopts.h) must be found before SDK defaults
target_include_directories(pico_bridge PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(pico_bridge
    pico_stdlib
    pico_unique_id
    hardware_uart
    hardware_dma
    hardware_irq
    tinyusb_device
    tinyusb_board
    pico_lwip
    pico_lwip_nosys
)

# Disable stdio on UART (we own UART0 for PPP) and USB (TinyUSB owns USB)
pico_enable_stdio_uart(pico_bridge 0)
pico_enable_stdio_usb(pico_bridge 0)

pico_add_extra_outputs(pico_bridge)
