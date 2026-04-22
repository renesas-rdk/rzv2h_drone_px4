# Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
#
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

project(CustomXRCEAgent VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Export compile_commands.json" FORCE)

# Set C++ standard (17 required by DRP-AI TVM headers)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Configuration info
message(STATUS "=== Custom XRCE Agent Configuration ===")
message(STATUS "CMAKE_TOOLCHAIN_FILE: ${CMAKE_TOOLCHAIN_FILE}")
message(STATUS "CMAKE_SYSROOT: ${CMAKE_SYSROOT}")
message(STATUS "CMAKE_C_COMPILER: ${CMAKE_C_COMPILER}")
message(STATUS "CMAKE_CXX_COMPILER: ${CMAKE_CXX_COMPILER}")

# Define paths to the built Micro-XRCE-DDS-Agent
set(MICRO_XRCE_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Micro-XRCE-DDS-Agent/build")
set(MICRO_XRCE_TEMP_INSTALL "${MICRO_XRCE_BUILD_DIR}/temp_install")

# Check if the Micro-XRCE-DDS-Agent was built
if(NOT EXISTS "${MICRO_XRCE_BUILD_DIR}/libmicroxrcedds_agent.so")
    message(FATAL_ERROR "Micro-XRCE-DDS-Agent not built! Please build it first using the provided commands.")
endif()

# Find all source files in current directory
file(GLOB AGENT_SOURCES "*.cpp" "*.c")

# Print source files for debugging
message(STATUS "Custom Agent Source Files:")
foreach(src ${AGENT_SOURCES})
    get_filename_component(name ${src} NAME)
    message(STATUS "  - ${name}")
endforeach()

# Create the custom agent executable using our real custom_agent.cpp
# Note: global_vars.c is excluded since custom_agent.cpp now contains all needed globals and functions
add_executable(CustomXRCEAgent
    custom_agent.cpp
    px4_openamp_rpc_server.cpp
    param_converter.cpp
    helper.c
    rz_rproc.c
    platform_info.c
    rz_init.c
)

# Include directories - use the built libraries' includes
target_include_directories(CustomXRCEAgent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    # Micro-XRCE-DDS-Agent includes
    ${CMAKE_CURRENT_SOURCE_DIR}/Micro-XRCE-DDS-Agent/include
    ${MICRO_XRCE_BUILD_DIR}/include  # For generated config.hpp
    # FastCDR includes (prefer installed headers which include generated config.h)
    ${MICRO_XRCE_TEMP_INSTALL}/fastcdr-2.2.4/include
    ${MICRO_XRCE_BUILD_DIR}/fastcdr/src/fastcdr/include
    ${MICRO_XRCE_BUILD_DIR}/fastcdr/src/fastcdr-build/include
    # FastDDS includes  
    ${MICRO_XRCE_BUILD_DIR}/fastdds/src/fastdds/include
    ${MICRO_XRCE_BUILD_DIR}/fastdds/src/fastdds-build/include
    # spdlog includes
    ${MICRO_XRCE_BUILD_DIR}/spdlog/src/spdlog/include
    # foonathan_memory includes
    ${MICRO_XRCE_BUILD_DIR}/foonathan_memory/src/foonathan_memory/include
    # System includes
    ${CMAKE_SYSROOT}/usr/include
    ${CMAKE_SYSROOT}/usr/include/openamp
    ${CMAKE_SYSROOT}/usr/include/libmetal
)

# Link directories
target_link_directories(CustomXRCEAgent PRIVATE
    ${MICRO_XRCE_BUILD_DIR}
    # Prefer installed libraries under temp_install
    ${MICRO_XRCE_TEMP_INSTALL}/fastcdr-2.2.4/lib
    ${MICRO_XRCE_TEMP_INSTALL}/fastdds-3.1/lib
    ${MICRO_XRCE_TEMP_INSTALL}/spdlog-1.9.2/lib
    ${MICRO_XRCE_TEMP_INSTALL}/foonathan_memory/lib
    # Fallback to build dirs
    ${MICRO_XRCE_BUILD_DIR}/fastcdr/src/fastcdr-build/src/cpp
    ${MICRO_XRCE_BUILD_DIR}/fastdds/src/fastdds-build/src/cpp
    ${MICRO_XRCE_BUILD_DIR}/spdlog/src/spdlog-build
    ${MICRO_XRCE_BUILD_DIR}/foonathan_memory/src/foonathan_memory-build/src
)

# Link against the built libraries in correct order
target_link_libraries(CustomXRCEAgent PRIVATE
    # Main Micro-XRCE-DDS-Agent library
    microxrcedds_agent
    # Dependencies in correct order
    fastdds
    fastcdr
    spdlog
    foonathan_memory-0.7.3
    # System libraries for OpenAMP
    open_amp
    metal
    # Standard system libraries
    pthread
    rt
    dl
)

# Set target properties
set_target_properties(CustomXRCEAgent PROPERTIES
    OUTPUT_NAME "CustomXRCEAgent"
)

# ── AI Camera (YOLOv8n + DRP-AI) ──────────────────────────────────
option(ENABLE_AI_CAMERA "Enable YOLOv8n AI Camera thread (DRP-AI)" OFF)

if(ENABLE_AI_CAMERA)
    # DRP-AI TVM SDK paths (headers + runtime libraries)
    set(RZV_DRP_TVM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../rzv_drp-ai_tvm")
    set(DRP_RUNTIME_LIB_DIR "${RZV_DRP_TVM_DIR}/obj/build_runtime/v2h/lib")

    if(NOT EXISTS "${DRP_RUNTIME_LIB_DIR}/libmera2_runtime.so")
        message(FATAL_ERROR "DRP-AI runtime not found at ${DRP_RUNTIME_LIB_DIR}. "
                            "Clone rzv_drp-ai_tvm with pre-built V2H runtime.")
    endif()

    target_sources(CustomXRCEAgent PRIVATE
        ai/yolo_detector.cpp
        ai/camera_capture.cpp
        ai/obstacle_processor.cpp
        ai/ai_thread.cpp
        ai/gst_h264_streamer.cpp
        ai/MeraDrpRuntimeWrapper.cpp
        ai/PreRuntimeV2H.cpp
    )
    target_compile_definitions(CustomXRCEAgent PRIVATE
        ENABLE_AI_CAMERA=1
        MERA_DRP_RUNTIME
        KDLDRPAI
    )
    # NEON FP16 — shared flag definition (see ai/neon_fp16.cmake for details)
    include(${CMAKE_CURRENT_SOURCE_DIR}/ai/neon_fp16.cmake)
    target_compile_options(CustomXRCEAgent PRIVATE ${NEON_FP16_FLAGS})

    # ── GStreamer (H.264 HW encoder → QGC stream) ─────────────────────
    # Poky SDK puts GStreamer in sysroot/usr/lib64 — locate via pkg-config or
    # hardcoded sysroot-relative paths if cross-compiling without pkg-config.
    if(CMAKE_SYSROOT)
        set(GST_INC_DIR  "${CMAKE_SYSROOT}/usr/include/gstreamer-1.0")
        set(GLIB_INC_DIR "${CMAKE_SYSROOT}/usr/include/glib-2.0")
        set(GLIB_CFG_DIR "${CMAKE_SYSROOT}/usr/lib64/glib-2.0/include")
        set(GST_LIB_DIR  "${CMAKE_SYSROOT}/usr/lib64")
    else()
        # Native build (on-board or x86 dev) — use system pkg-config paths
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0)
        set(GST_INC_DIR  ${GST_INCLUDE_DIRS})
        set(GLIB_INC_DIR "")
        set(GLIB_CFG_DIR "")
        set(GST_LIB_DIR  ${GST_LIBRARY_DIRS})
    endif()

    target_include_directories(CustomXRCEAgent PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ai
        ${CMAKE_CURRENT_SOURCE_DIR}/ai/include
        ${RZV_DRP_TVM_DIR}/apps/include
        ${RZV_DRP_TVM_DIR}/setup/include
        # asio (header-only, used by mera_runtime.h)
        ${MICRO_XRCE_BUILD_DIR}/fastdds/src/fastdds/thirdparty/asio/asio/include
        # GStreamer / GLib headers
        ${GST_INC_DIR}
        ${GLIB_INC_DIR}
        ${GLIB_CFG_DIR}
    )
    target_link_directories(CustomXRCEAgent PRIVATE ${GST_LIB_DIR})
    # libonnxruntime.so is only present in newer TVM builds — link if available
    set(ONNXRT_LIB "${DRP_RUNTIME_LIB_DIR}/libonnxruntime.so")
    if(EXISTS "${ONNXRT_LIB}")
        set(ONNXRT_LINK "${ONNXRT_LIB}")
    else()
        set(ONNXRT_LINK "")
        message(STATUS "libonnxruntime.so not found — skipping (older TVM build)")
    endif()

    target_link_libraries(CustomXRCEAgent PRIVATE
        ${DRP_RUNTIME_LIB_DIR}/libmera2_runtime.so
        ${DRP_RUNTIME_LIB_DIR}/libacl_rt.so
        ${DRP_RUNTIME_LIB_DIR}/libdrp_tvm_rt.so
        ${DRP_RUNTIME_LIB_DIR}/libdrp_rt.so
        ${DRP_RUNTIME_LIB_DIR}/libarm_compute.so
        ${DRP_RUNTIME_LIB_DIR}/libarm_compute_core.so
        ${DRP_RUNTIME_LIB_DIR}/libarm_compute_graph.so
        ${DRP_RUNTIME_LIB_DIR}/libmera2_plan_io.so
        ${ONNXRT_LINK}
        turbojpeg
        jpeg
        gstreamer-1.0
        gstapp-1.0
        gstbase-1.0
        gobject-2.0
        glib-2.0
    )
    message(STATUS "ENABLE_AI_CAMERA=ON — YOLOv8n DRP-AI + QGC H.264 stream enabled")
endif()

# Print build information
message(STATUS "=== Build Configuration ===")
message(STATUS "Micro-XRCE Build Dir: ${MICRO_XRCE_BUILD_DIR}")
message(STATUS "Output Binary: CustomXRCEAgent")
message(STATUS "Using real custom_agent.cpp from src/ directory")
