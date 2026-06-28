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
    # Try to locate AAudio. If present, link it and define HAVE_AAUDIO.
    find_library(AAUDIO_LIB NAMES aaudio)

    if(AAUDIO_LIB)
        message(STATUS "Found AAudio: ${AAUDIO_LIB}; linking AAudio")
        target_link_libraries(${PROJECT_NAME}
            geode-sdk
            mediandk       # AMediaCodec, AMediaMuxer — video encoding
            ${AAUDIO_LIB}  # AAudio — microphone capture (only when present)
            GLESv2         # glReadPixels — frame capture
            android
            log
        )
        target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_AAUDIO=1)
    else()
        message(WARNING "AAudio not found; building without AAudio support")
        target_link_libraries(${PROJECT_NAME}
            geode-sdk
            mediandk       # AMediaCodec, AMediaMuxer — video encoding
            GLESv2         # glReadPixels — frame capture
            android
            log
        )
        target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_AAUDIO=0)
    endif()
else()
    target_link_libraries(${PROJECT_NAME} geode-sdk)
endif()

setup_geode_mod(${PROJECT_NAME})
