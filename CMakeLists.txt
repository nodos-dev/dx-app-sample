# Copyright MediaZ Teknoloji A.S. All Rights Reserved.

cmake_minimum_required(VERSION 3.10)

project(NosDxAppSample)

# Dependencies
# SDL2 (Minimal setup)
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
set(SDL_LOADSO ON  CACHE BOOL "" FORCE)
set(SDL_VIDEO ON  CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_STATIC_PIC ON CACHE BOOL "" FORCE)
add_subdirectory(External/SDL EXCLUDE_FROM_ALL)

# DirectX-Headers
add_subdirectory(External/DirectX-Headers EXCLUDE_FROM_ALL)

if (DEFINED NOSMAN_WORKSPACE_DIR)
    # Use nosman to get SDK PATH
    set(NOSMAN_PATH ${NOSMAN_WORKSPACE_DIR}/nodos${CMAKE_EXECUTABLE_SUFFIX})
    cmake_path(SET NOSMAN_PATH ${NOSMAN_PATH} NORMALIZE)
    execute_process(COMMAND ${NOSMAN_PATH} --workspace "${NOSMAN_WORKSPACE_DIR}" sdk-info "1.2.0" OUTPUT_VARIABLE NOSMAN_SDK_INFO)
    string(JSON NODOS_SDK_DIR GET ${NOSMAN_SDK_INFO} "path")
    string(STRIP ${NODOS_SDK_DIR} NODOS_SDK_DIR)
    set(NODOS_SDK_DIR ${NODOS_SDK_DIR} CACHE PATH "Path to Nodos SDK")
    # Make path cross-platform
    string(REPLACE "\\" "/" NODOS_SDK_DIR ${NODOS_SDK_DIR})
else()
    if (NOT DEFINED NODOS_SDK_DIR)
        message(FATAL_ERROR "NODOS_SDK_DIR or NOSMAN_WORKSPACE_DIR is not defined")
    endif()
endif()
list(APPEND CMAKE_MODULE_PATH ${NODOS_SDK_DIR}/cmake)
add_library(nosAppSDK INTERFACE)
target_include_directories(nosAppSDK INTERFACE ${NODOS_SDK_DIR}/include)

set(CMAKE_CXX_STANDARD 20)
file(GLOB_RECURSE SOURCES Source/*.cpp Source/*.hpp)
add_executable(NosDxAppSample ${SOURCES})
target_include_directories(NosDxAppSample PRIVATE Source)
target_link_libraries(NosDxAppSample PRIVATE nosAppSDK d3d12 dxgi d3dcompiler SDL2-static DirectX-Headers Shlwapi.lib)
target_compile_definitions(NosDxAppSample PRIVATE NODOS_APP_SDK_DLL="${NODOS_SDK_DIR}/bin/nosAppSDK.dll")
