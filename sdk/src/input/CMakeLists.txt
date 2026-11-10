# rexinput - Input system library
# Input system and drivers for Xbox 360 controller emulation

add_library(rexinput OBJECT
    input_system.cpp
    device_assignment.cpp
    slot_table.cpp
    state_log.cpp
    state_merge.cpp
    mnk/mnk_input_driver.cpp
    nop/nop_input_driver.cpp
)
add_library(rex::input ALIAS rexinput)

rex_target_pch(rexinput
    <mutex>
    <rex/math.h>
    <rex/string.h>
    <rex/memory.h>
    <rex/filesystem.h>
    <rex/thread.h>
    <rex/thread/mutex.h>
    <rex/ppc/context.h>
    <rex/system/kernel_state.h>
    <rex/system/xthread.h>
    <rex/ui/window.h>
    <rex/ui/presenter.h>
)

target_include_directories(rexinput PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
if(REXGLUE_PLATFORM_SWITCH)
    target_sources(rexinput PRIVATE switch/switch_input_driver.cpp)
elseif(WIN32)
    target_sources(rexinput PRIVATE sdl/sdl_input_driver.cpp xinput/xinput_input_driver.cpp)
else()
    target_sources(rexinput PRIVATE sdl/sdl_input_driver.cpp)
endif()
# Scripted virtual pads and Steam Input detection (mac-input): desktop only.
if(NOT REXGLUE_PLATFORM_SWITCH)
    target_sources(rexinput PRIVATE script/script_events.cpp script/script_input_driver.cpp
        steam_detect.cpp)
endif()
# USB port lookup for pads without a serial, used by the SDL driver.
if(APPLE)
    target_sources(rexinput PRIVATE sdl/pad_location_mac.mm)
    set_source_files_properties(sdl/pad_location_mac.mm PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(rexinput PUBLIC "-framework GameController" "-framework IOKit")
elseif(NOT REXGLUE_PLATFORM_SWITCH)
    target_sources(rexinput PRIVATE sdl/pad_location_stub.cpp)
endif()

target_link_libraries(rexinput PUBLIC rexcore rexui)
if(NOT REXGLUE_PLATFORM_SWITCH)
    target_link_libraries(rexinput PUBLIC SDL3::SDL3)
endif()
