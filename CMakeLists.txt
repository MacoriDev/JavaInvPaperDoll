cmake_minimum_required(VERSION 3.22)

project(InventoryTouchFollowProbe LANGUAGES CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "Android NDK toolchain is required.")
endif()

if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "Only arm64-v8a is supported for the located Minecraft Bedrock hooks.")
endif()

add_library(InventoryTouchFollowProbe SHARED
    src/main.cpp
)

target_compile_features(InventoryTouchFollowProbe PRIVATE cxx_std_20)
target_compile_options(InventoryTouchFollowProbe PRIVATE
    -O2
    -Wall
    -Wextra
    -Wpedantic
    -fvisibility=hidden
    -ffunction-sections
    -fdata-sections
)
target_link_options(InventoryTouchFollowProbe PRIVATE
    -Wl,--gc-sections
    -Wl,--strip-all
)

find_library(log_lib log REQUIRED)
find_library(android_lib android REQUIRED)
find_library(dl_lib dl REQUIRED)

target_link_libraries(InventoryTouchFollowProbe PRIVATE
    ${log_lib}
    ${android_lib}
    ${dl_lib}
)

set_target_properties(InventoryTouchFollowProbe PROPERTIES
    OUTPUT_NAME "InventoryTouchFollowProbe"
)

# Runtime dependency only:
# LeviLauncher supplies libpreloader.so. This test module resolves GlossHook
# APIs from that already-loaded preloader at runtime, so CI does not rebuild or
# redistribute a separate preloader binary.
