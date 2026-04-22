# Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
#
# SPDX-License-Identifier: BSD-3-Clause

# Toolchain Configuration - Central location for toolchain paths
# This file defines the toolchain paths used by both main project and px4 subproject

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR ARM)

# Handle TOOLCHAIN_BASE_PATH environment variable
if(DEFINED ENV{TOOLCHAIN_BASE_PATH})
    set(TOOLCHAIN_BASE_PATH $ENV{TOOLCHAIN_BASE_PATH})
else()
    set(TOOLCHAIN_BASE_PATH "/opt/toolchains/gcc_arm/13_3-Rel1")
    message(WARNING "TOOLCHAIN_BASE_PATH not set, using default: ${TOOLCHAIN_BASE_PATH}")
endif()

if(DEFINED ENV{GNUARM_TOOLCHAIN_ROOT})
    set(GNUARM_TOOLCHAIN_ROOT $ENV{GNUARM_TOOLCHAIN_ROOT})
elseif(DEFINED ENV{GNUARMEMB_TOOLCHAIN_PATH})
    set(GNUARM_TOOLCHAIN_ROOT $ENV{GNUARMEMB_TOOLCHAIN_PATH})
else()
    set(GNUARM_TOOLCHAIN_ROOT "${TOOLCHAIN_BASE_PATH}/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi")
endif()

set(GNUARM_TOOLCHAIN_ROOT "${GNUARM_TOOLCHAIN_ROOT}" CACHE PATH "GNU Arm Embedded toolchain root" FORCE)
set(GNUARMEMB_TOOLCHAIN_PATH "${GNUARM_TOOLCHAIN_ROOT}" CACHE PATH "GNU Arm Embedded toolchain" FORCE)

set(CMAKE_FIND_ROOT_PATH "${GNUARM_TOOLCHAIN_ROOT}/bin")
message(STATUS "Using toolchain at: ${GNUARM_TOOLCHAIN_ROOT}")

if(NOT CMAKE_FIND_ROOT_PATH MATCHES "/$")
    set(CMAKE_FIND_ROOT_PATH "${CMAKE_FIND_ROOT_PATH}/")
endif()

if(CMAKE_HOST_WIN32)
    set(BINARY_FILE_EXT ".exe")
else()
    set(BINARY_FILE_EXT "")
endif()

set(CMAKE_C_COMPILER ${CMAKE_FIND_ROOT_PATH}arm-none-eabi-gcc${BINARY_FILE_EXT})
set(CMAKE_C_COMPILER_ID GCCRZ)
set(CMAKE_C_COMPILER_ID_RUN TRUE)
set(CMAKE_C_COMPILER_FORCED TRUE)

set(CMAKE_CXX_COMPILER ${CMAKE_FIND_ROOT_PATH}arm-none-eabi-g++${BINARY_FILE_EXT})
set(CMAKE_CXX_COMPILER_ID GCCRZ)
set(CMAKE_CXX_COMPILER_ID_RUN TRUE)
set(CMAKE_CXX_COMPILER_FORCED TRUE)

set(CMAKE_ASM_COMPILER ${CMAKE_FIND_ROOT_PATH}arm-none-eabi-gcc${BINARY_FILE_EXT})
set(CMAKE_ASM_COMPILER_FORCED TRUE)
set(CMAKE_LINKER ${CMAKE_FIND_ROOT_PATH}arm-none-eabi-gcc${BINARY_FILE_EXT})
set(CMAKE_OBJCOPY ${CMAKE_FIND_ROOT_PATH}arm-none-eabi-objcopy${BINARY_FILE_EXT})
set(CMAKE_SIZE ${CMAKE_FIND_ROOT_PATH}arm-none-eabi-size${BINARY_FILE_EXT})

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(ARM_CPU "cortex-r8")
set(ARM_FPU "vfpv3-d16")
set(ARM_FLOAT_ABI "hard")

set(ARM_COMMON_FLAGS "-mthumb -mcpu=${ARM_CPU} -mfloat-abi=${ARM_FLOAT_ABI} -mfpu=${ARM_FPU}")

# Add stack usage analysis flags (disabled -Werror for now to allow build)
# Set threshold to 2560 bytes (above control allocation but catches FSP driver issues)
set(STACK_ANALYSIS_FLAGS "-fstack-usage")

set(CMAKE_C_FLAGS_INIT "${ARM_COMMON_FLAGS} ${STACK_ANALYSIS_FLAGS} -ffunction-sections -fdata-sections -fno-strict-aliasing -fno-unwind-tables -fno-asynchronous-unwind-tables -Wall -Wextra")
set(CMAKE_CXX_FLAGS_INIT "${ARM_COMMON_FLAGS} ${STACK_ANALYSIS_FLAGS} -ffunction-sections -fdata-sections -fno-strict-aliasing -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -fno-unwind-tables -fno-asynchronous-unwind-tables -fexceptions -Wall -Wextra")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${ARM_COMMON_FLAGS} -Wl,--gc-sections -specs=nosys.specs")

if(NOT DEFINED CMAKE_C_FLAGS_DEBUG OR CMAKE_C_FLAGS_DEBUG STREQUAL "")
    set(CMAKE_C_FLAGS_DEBUG "-g3 -Og" CACHE STRING "Debug compile flags for C" FORCE)
endif()

if(NOT DEFINED CMAKE_CXX_FLAGS_DEBUG OR CMAKE_CXX_FLAGS_DEBUG STREQUAL "")
    set(CMAKE_CXX_FLAGS_DEBUG "-g3 -Og" CACHE STRING "Debug compile flags for CXX" FORCE)
endif()

if(NOT DEFINED CMAKE_ASM_FLAGS_DEBUG OR CMAKE_ASM_FLAGS_DEBUG STREQUAL "")
    set(CMAKE_ASM_FLAGS_DEBUG "-g3" CACHE STRING "Debug compile flags for ASM" FORCE)
endif()

# Set Release optimization flags to prevent stack overflow
if(NOT DEFINED CMAKE_C_FLAGS_RELEASE OR CMAKE_C_FLAGS_RELEASE STREQUAL "")
    set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "Release compile flags for C" FORCE)
endif()

if(NOT DEFINED CMAKE_CXX_FLAGS_RELEASE OR CMAKE_CXX_FLAGS_RELEASE STREQUAL "")
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "Release compile flags for CXX" FORCE)
endif()

if(NOT DEFINED CMAKE_ASM_FLAGS_RELEASE OR CMAKE_ASM_FLAGS_RELEASE STREQUAL "")
    set(CMAKE_ASM_FLAGS_RELEASE "" CACHE STRING "Release compile flags for ASM" FORCE)
endif()

# Set RelWithDebInfo flags
if(NOT DEFINED CMAKE_C_FLAGS_RELWITHDEBINFO OR CMAKE_C_FLAGS_RELWITHDEBINFO STREQUAL "")
    set(CMAKE_C_FLAGS_RELWITHDEBINFO "-g3 -O2 -DNDEBUG" CACHE STRING "RelWithDebInfo compile flags for C" FORCE)
endif()

if(NOT DEFINED CMAKE_CXX_FLAGS_RELWITHDEBINFO OR CMAKE_CXX_FLAGS_RELWITHDEBINFO STREQUAL "")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-g3 -O2 -DNDEBUG" CACHE STRING "RelWithDebInfo compile flags for CXX" FORCE)
endif()

# Set MinSizeRel flags  
if(NOT DEFINED CMAKE_C_FLAGS_MINSIZEREL OR CMAKE_C_FLAGS_MINSIZEREL STREQUAL "")
    set(CMAKE_C_FLAGS_MINSIZEREL "-Os -DNDEBUG" CACHE STRING "MinSizeRel compile flags for C" FORCE)
endif()

if(NOT DEFINED CMAKE_CXX_FLAGS_MINSIZEREL OR CMAKE_CXX_FLAGS_MINSIZEREL STREQUAL "")
    set(CMAKE_CXX_FLAGS_MINSIZEREL "-Os -DNDEBUG" CACHE STRING "MinSizeRel compile flags for CXX" FORCE)
endif()
