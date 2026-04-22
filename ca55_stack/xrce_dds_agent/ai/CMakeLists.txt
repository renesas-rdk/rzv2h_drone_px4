cmake_minimum_required(VERSION 3.24)
set(CMAKE_CXX_STANDARD 17)
project(ai_camera_test)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(default_build_type "Release")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE STRING "Build type" FORCE)
endif()

# TVM_ROOT must be set (e.g. /drp-ai_tvm)
if(NOT DEFINED ENV{TVM_ROOT})
  message(FATAL_ERROR "TVM_ROOT not set. Source the SDK env and set TVM_ROOT=/drp-ai_tvm")
endif()

set(TVM_ROOT "$ENV{TVM_ROOT}")

# Include paths
include_directories(${TVM_ROOT}/tvm/include)
include_directories(${TVM_ROOT}/setup/include)
include_directories(${TVM_ROOT}/tvm/3rdparty/dlpack/include)
include_directories(${TVM_ROOT}/tvm/3rdparty/dmlc-core/include)
include_directories(${TVM_ROOT}/tvm/3rdparty/compiler-rt)
include_directories(${TVM_ROOT}/apps/include)
include_directories(${TVM_ROOT}/3rdparty/spdlog/include)
include_directories(${TVM_ROOT}/3rdparty/asio/asio/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# V2H runtime library path
set(LIBMERA_RT_PATH ${TVM_ROOT}/obj/build_runtime/v2h/lib)

# Source files
set(SRC
    ai_camera_test.cpp
    camera_capture.cpp
    obstacle_processor.cpp
    yolo_detector.cpp
    MeraDrpRuntimeWrapper.cpp
    PreRuntimeV2H.cpp
)

set(EXE_NAME ai_camera_test)

# Linker flags
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath,${LIBMERA_RT_PATH} -Wl,-rpath-link,${LIBMERA_RT_PATH}")

add_executable(${EXE_NAME} ${SRC})

target_link_directories(${EXE_NAME} PRIVATE ${LIBMERA_RT_PATH})

find_package(OpenCV REQUIRED)
target_link_libraries(${EXE_NAME} PRIVATE ${OpenCV_LIBS})

target_link_libraries(${EXE_NAME} PRIVATE
    mera2_runtime
    mera2_plan_io
    drp_tvm_rt
    pthread
)

# V2H uses DRP-AI accelerator
add_definitions(-DMERA_DRP_RUNTIME)
target_compile_definitions(${EXE_NAME} PUBLIC KDLDRPAI)

target_compile_options(${EXE_NAME} PRIVATE -O3 -mtune=cortex-a55 -Wall)

# NEON FP16 — shared flag definition (see neon_fp16.cmake for details)
include(${CMAKE_CURRENT_SOURCE_DIR}/neon_fp16.cmake)
target_compile_options(${EXE_NAME} PRIVATE ${NEON_FP16_FLAGS})

# Minimal test binary (same as official run_inference_v2h)
add_executable(minimal_test
    minimal_test.cpp
    MeraDrpRuntimeWrapper.cpp
    PreRuntimeV2H.cpp
)
target_link_directories(minimal_test PRIVATE ${LIBMERA_RT_PATH})
target_link_libraries(minimal_test PRIVATE mera2_runtime mera2_plan_io drp_tvm_rt pthread)
target_compile_definitions(minimal_test PUBLIC KDLDRPAI)
target_compile_options(minimal_test PRIVATE -O3 -mtune=cortex-a55 -Wall)
