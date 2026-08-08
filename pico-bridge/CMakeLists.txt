cmake_minimum_required(VERSION 3.20)

include(pico_sdk_import.cmake)

project(pico_ncm_ppp_bridge C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pico_ncm_ppp_bridge
    src/main.c
    src/usb_descriptors.c
)

target_include_directories(pico_ncm_ppp_bridge PRIVATE src)

target_link_libraries(pico_ncm_ppp_bridge
    pico_stdlib
    pico_unique_id
    hardware_uart
    hardware_irq
    tinyusb_device
    tinyusb_board
    pico_lwip_core
    pico_lwip_core4
    pico_lwip_core6
    pico_lwip_netif
    pico_lwip_ppp
    pico_lwip_nosys
)

# Disable stdio over USB (USB is for NCM network)
pico_enable_stdio_usb(pico_ncm_ppp_bridge 0)
pico_enable_stdio_uart(pico_ncm_ppp_bridge 1)

pico_add_extra_outputs(pico_ncm_ppp_bridge)
