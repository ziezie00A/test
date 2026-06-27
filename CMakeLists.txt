cmake_minimum_required(VERSION 3.21)
project(MacroSafeguard)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(${PROJECT_NAME} SHARED src/main.cpp)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK.")
else()
    message(STATUS "Found Geode SDK at $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

if (ANDROID)
    target_link_libraries(${PROJECT_NAME}
        geode-sdk
        mediandk       # AMediaCodec, AMediaMuxer, AMediaFormat
        GLESv2         # glReadPixels
        android
        log
    )
else()
    target_link_libraries(${PROJECT_NAME} geode-sdk)
endif()

setup_geode_mod(${PROJECT_NAME})
