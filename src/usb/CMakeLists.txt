
add_library(sd2psx_usb STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/usb_descriptors.c
  ${CMAKE_CURRENT_SOURCE_DIR}/usb_device.c
  ${CMAKE_CURRENT_SOURCE_DIR}/msc_disk.c
  ${CMAKE_CURRENT_SOURCE_DIR}/msc_mode.c
)

# Il nostro tusb_config.h deve avere la precedenza su quello di pico_stdio_usb:
# per questo il target NON linka pico_stdio_usb ma tinyusb_device.
target_include_directories(sd2psx_usb
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(sd2psx_usb
  PUBLIC
    tinyusb_device
  PRIVATE
    pico_stdlib
    pico_stdio
    pico_unique_id
    hardware_watchdog
    sd_fat
    sd2psx_common
)

if (SD2PSX_WITH_LED)
  target_link_libraries(sd2psx_usb PRIVATE sd2psx_led)
endif()

target_compile_definitions(sd2psx_usb PUBLIC "WITH_MSC=1")
