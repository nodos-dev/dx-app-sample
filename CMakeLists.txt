# Copyright MediaZ Teknoloji A.S. All Rights Reserved.

cmake_minimum_required(VERSION 3.14) # Required for FetchContent

project(NosDxAppSample)

include(FetchContent)

# SDL2 (Minimal setup)
FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.32.8
)
FetchContent_MakeAvailable(SDL2)

# Set SDL configuration flags
set(SDL_ATOMIC OFF CACHE BOOL "" FORCE)
set(SDL_CPUINFO OFF CACHE BOOL "" FORCE)
set(SDL_EVENTS ON CACHE BOOL "" FORCE)
set(SDL_FILE OFF CACHE BOOL "" FORCE)
set(SDL_FILESYSTEM OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_LOCALE OFF CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_TIMERS OFF CACHE BOOL "" FORCE)
set(SDL_DLOPEN OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_LOADSO ON CACHE BOOL "" FORCE)
set(SDL_VIDEO ON CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_STATIC_PIC ON CACHE BOOL "" FORCE)

# DirectX-Headers
FetchContent_Declare(
    DirectX-Headers
    GIT_REPOSITORY https://github.com/microsoft/DirectX-Headers.git
    GIT_TAG v1.616.0
)
FetchContent_MakeAvailable(DirectX-Headers)

# Nodos SDK Configuration
if (DEFINED NODOS_WORKSPACE_DIR)
    set(NOSMAN_WORKSPACE_DIR ${NODOS_WORKSPACE_DIR})
    set(NOSMAN_EXECUTABLE ${NODOS_WORKSPACE_DIR}/nodos${CMAKE_EXECUTABLE_SUFFIX})
    cmake_path(SET NOSMAN_EXECUTABLE ${NOSMAN_EXECUTABLE} NORMALIZE)
    execute_process(
        COMMAND ${NOSMAN_EXECUTABLE} --workspace "${NODOS_WORKSPACE_DIR}" sdk-info "20.0" "process"
        OUTPUT_VARIABLE NOSMAN_SDK_INFO
    )
    string(JSON NODOS_SDK_DIR GET ${NOSMAN_SDK_INFO} "path")
    string(STRIP ${NODOS_SDK_DIR} NODOS_SDK_DIR)
    set(NODOS_SDK_DIR ${NODOS_SDK_DIR} CACHE PATH "Path to Nodos SDK")
    string(REPLACE "\\" "/" NODOS_SDK_DIR ${NODOS_SDK_DIR})
else()
    message(FATAL_ERROR "NODOS_WORKSPACE_DIR is not defined")
endif()

list(APPEND CMAKE_MODULE_PATH ${NODOS_SDK_DIR}/cmake)

if (NOT EXISTS ${NODOS_WORKSPACE_DIR}/Toolchain/CMake)
    message(FATAL_ERROR "Nodos CMake tools not found in workspace")
endif()

include(${NODOS_WORKSPACE_DIR}/Toolchain/CMake/Scripts/Utils.cmake)
include(${NODOS_WORKSPACE_DIR}/Toolchain/CMake/Scripts/Projects.cmake)

# Get nos.sys.vulkan
nos_get_module("nos.sys.vulkan" "6.0" NOS_SYS_VULKAN_TARGET)
message(STATUS "nos.sys.vulkan target: ${NOS_SYS_VULKAN_TARGET}")

add_library(nosAppSDK INTERFACE)
target_include_directories(nosAppSDK INTERFACE ${NODOS_SDK_DIR}/include)

set(CMAKE_CXX_STANDARD 20)

file(GLOB_RECURSE SOURCES Source/*.cpp Source/*.hpp)

add_executable(NosDxAppSample ${SOURCES})
target_include_directories(NosDxAppSample PRIVATE Source)
target_link_libraries(NosDxAppSample PRIVATE
    nosAppSDK
    ${NOS_SYS_VULKAN_TARGET}
    d3d12 dxgi d3dcompiler
    SDL2-static
    DirectX-Headers
    Shlwapi.lib
)

target_compile_definitions(NosDxAppSample PRIVATE NODOS_APP_SDK_DLL="${NODOS_SDK_DIR}/bin/nosAppSDK.dll")

set_target_properties(NosDxAppSample PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Binaries"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/Binaries"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/Binaries"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_CURRENT_SOURCE_DIR}/Binaries"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_CURRENT_SOURCE_DIR}/Binaries"
)
