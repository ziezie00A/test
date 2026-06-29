cmake_minimum_required(VERSION 3.21)
project(MacroSafeguard)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(${PROJECT_NAME} SHARED src/main.cpp)

# Applying this target-wide fixes the Precompiled Header dialect mismatch
if (APPLE)
    target_compile_options(${PROJECT_NAME} PRIVATE "-x" "objective-c++")
endif()

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK.")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

if (ANDROID)
    target_link_libraries(${PROJECT_NAME}
        geode-sdk
        mediandk       
        GLESv2         
        android
        log
    )
elseif (WIN32)
    target_link_libraries(${PROJECT_NAME} 
        geode-sdk
        mfplat
        mfuuid
        mfreadwrite
        ole32
    )
elseif (APPLE)
    find_library(AVFOUNDATION_RAW AVFoundation)
    find_library(COREMEDIA_RAW CoreMedia)
    find_library(COREVIDEO_RAW CoreVideo)
    
    target_link_libraries(${PROJECT_NAME} 
        geode-sdk 
        ${AVFOUNDATION_RAW} 
        ${COREMEDIA_RAW} 
        ${COREVIDEO_RAW}
    )
endif()

setup_geode_mod(${PROJECT_NAME})

