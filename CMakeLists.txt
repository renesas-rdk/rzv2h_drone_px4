cmake_minimum_required(VERSION 3.16.4)

if(POLICY CMP0148)
    cmake_policy(SET CMP0148 OLD)
endif()

# Set the toolchain file before project declaration
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cross.cmake)

project(rzv2h_px4_freertos VERSION 1.0.0 LANGUAGES C CXX ASM)

set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON)

set(CMAKE_ASM_SOURCE_FILE_EXTENSIONS s;S;asm)

add_compile_options(
    "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/freertos_posix/FreeRTOSCustomConfig.h"
)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

if(NOT DEFINED RZV_MAVLINK_USE_OPENAMP)
    set(RZV_MAVLINK_USE_OPENAMP OFF)
endif()
option(RZV_MAVLINK_USE_OPENAMP "Route MAVLink telemetry over OpenAMP instead of UART5/SIK" ${RZV_MAVLINK_USE_OPENAMP})
option(PX4_ENABLE_STACK_MONITOR "Enable PX4 stack monitor task" OFF)
option(CONFIG_RZV_MEM_TELEMETRY "Enable runtime heap/IRQ telemetry logging" OFF)
option(CONFIG_SYSCALL_DEBUG_VERBOSE "Enable verbose syscall logging" OFF)

set(PX4_DIR ${CMAKE_CURRENT_SOURCE_DIR}/px4)
set(PX4_BUILD_DIR ${PX4_DIR}/build/rzv)

set(MAVLINK_INCLUDE_DIR "${MAVLINK_INCLUDE_DIR}" CACHE PATH "MAVLink headers install/include directory")
if(NOT MAVLINK_INCLUDE_DIR)
    set(MAVLINK_INCLUDE_DIR "${PX4_DIR}/src/modules/mavlink/mavlink/install/include")
endif()

set(MAVLINK_DIALECT "${MAVLINK_DIALECT}" CACHE STRING "MAVLink dialect to build")
if(NOT MAVLINK_DIALECT)
    set(MAVLINK_DIALECT "common")
endif()

include(ProcessorCount)
ProcessorCount(_nproc)
if(_nproc GREATER 0)
    set(CMAKE_BUILD_PARALLEL_LEVEL ${_nproc})
    set(_parallel_jobs ${_nproc})
else()
    set(CMAKE_BUILD_PARALLEL_LEVEL 4)
    set(_parallel_jobs 4)
endif()
message(STATUS "Parallel build jobs: ${_parallel_jobs}")
message(STATUS "CA55 support: ON (required)")
message(STATUS "MAVLink via OpenAMP: ${RZV_MAVLINK_USE_OPENAMP}")
message(STATUS "PX4 stack monitor: ${PX4_ENABLE_STACK_MONITOR}")
message(STATUS "RZV memory telemetry: ${CONFIG_RZV_MEM_TELEMETRY}")
message(STATUS "Syscall debug verbose: ${CONFIG_SYSCALL_DEBUG_VERBOSE}")
message(STATUS "MAVLink dialect: ${MAVLINK_DIALECT}")
message(STATUS "MAVLink include dir: ${MAVLINK_INCLUDE_DIR}")

set(PX4_EXTRA_CMAKE_ARGS "${PX4_EXTRA_CMAKE_ARGS}" CACHE STRING "Additional PX4 configure arguments")

find_program(_ccache_program ccache)
set(_ccache_launcher "")
if(_ccache_program)
    if(DEFINED CCACHE_DIR AND NOT CCACHE_DIR STREQUAL "")
        set(_ccache_dir "${CCACHE_DIR}")
    else()
        set(_ccache_dir "${CMAKE_CURRENT_SOURCE_DIR}/.ccache")
    endif()

    file(MAKE_DIRECTORY "${_ccache_dir}")
    set(ENV{CCACHE_DIR} "${_ccache_dir}")

    set(_ccache_launcher env;CCACHE_DIR=${_ccache_dir};${_ccache_program})
    set(CMAKE_C_COMPILER_LAUNCHER ${_ccache_launcher})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${_ccache_launcher})

    message(STATUS "ccache found: ${_ccache_program} (CCACHE_DIR=${_ccache_dir})")
    execute_process(COMMAND ${_ccache_program} --set-config=max_size=2G)
    execute_process(COMMAND ${_ccache_program} --set-config=compression=true)
    execute_process(COMMAND ${_ccache_program} --set-config=compression_level=6)
else()
    message(STATUS "ccache not found, builds will be slower")
endif()

file(MAKE_DIRECTORY ${PX4_BUILD_DIR})

if(EXISTS ${PX4_DIR}/CMakeLists.txt)
    include(ExternalProject)

    set(_px4_cmake_args
        -G Ninja
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/cross.cmake
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_INSTALL_LIBDIR=lib
        -DPX4_BUILD_LIB_ONLY=ON
        -DPX4_BOARD=renesas_rzv_default
        -DCONFIG=renesas_rzv_default
        -DRZV_MAVLINK_USE_OPENAMP=${RZV_MAVLINK_USE_OPENAMP}
    )

    if(PX4_EXTRA_CMAKE_ARGS)
        separate_arguments(_px4_extra_args NATIVE_COMMAND "${PX4_EXTRA_CMAKE_ARGS}")
        list(APPEND _px4_cmake_args ${_px4_extra_args})
    endif()

    # Add verbose flag to PX4 build if CMAKE_VERBOSE_MAKEFILE is ON
    if(CMAKE_VERBOSE_MAKEFILE)
        set(_px4_build_cmd ${CMAKE_COMMAND} --build . --parallel ${_parallel_jobs} --verbose --target px4_lib)
    else()
        set(_px4_build_cmd ${CMAKE_COMMAND} --build . --parallel ${_parallel_jobs} --target px4_lib)
    endif()

    ExternalProject_Add(px4_external
        SOURCE_DIR ${PX4_DIR}
        BINARY_DIR ${PX4_BUILD_DIR}
        CMAKE_ARGS ${_px4_cmake_args}
        BUILD_COMMAND ${_px4_build_cmd}
        INSTALL_COMMAND ""
        USES_TERMINAL_CONFIGURE TRUE
        USES_TERMINAL_BUILD TRUE
    )

    add_custom_target(px4_lib_bundle ALL DEPENDS px4_external)

    add_library(px4_lib STATIC IMPORTED GLOBAL)
    set_target_properties(px4_lib PROPERTIES IMPORTED_LOCATION ${PX4_BUILD_DIR}/libpx4.a)
    add_dependencies(px4_lib px4_external)

    add_library(px4_uorb_msgs STATIC IMPORTED GLOBAL)
    set_target_properties(px4_uorb_msgs PROPERTIES IMPORTED_LOCATION ${PX4_BUILD_DIR}/msg/libuorb_msgs.a)
    add_dependencies(px4_uorb_msgs px4_external)

    add_library(px4_perf STATIC IMPORTED GLOBAL)
    set_target_properties(px4_perf PROPERTIES IMPORTED_LOCATION ${PX4_BUILD_DIR}/src/lib/perf/libperf.a)
    add_dependencies(px4_perf px4_external)

    add_library(px4_tinybson STATIC IMPORTED GLOBAL)
    set_target_properties(px4_tinybson PROPERTIES IMPORTED_LOCATION ${PX4_BUILD_DIR}/src/lib/tinybson/libtinybson.a)
    add_dependencies(px4_tinybson px4_external)

    add_library(px4_geo STATIC IMPORTED GLOBAL)
    set_target_properties(px4_geo PROPERTIES IMPORTED_LOCATION ${PX4_BUILD_DIR}/src/lib/geo/libgeo.a)
    add_dependencies(px4_geo px4_external)

    add_library(px4_modules_ekf2 STATIC IMPORTED GLOBAL)
    set_target_properties(px4_modules_ekf2 PROPERTIES IMPORTED_LOCATION ${PX4_BUILD_DIR}/src/modules/ekf2/libmodules__ekf2.a)
    add_dependencies(px4_modules_ekf2 px4_external)

    add_library(px4_output_predictor STATIC IMPORTED GLOBAL)
    set_target_properties(px4_output_predictor PROPERTIES IMPORTED_LOCATION ${PX4_BUILD_DIR}/src/modules/ekf2/EKF/output_predictor/liboutput_predictor.a)
    add_dependencies(px4_output_predictor px4_external)

    add_library(px4_lat_lon_alt STATIC IMPORTED GLOBAL)
    set_target_properties(px4_lat_lon_alt PROPERTIES IMPORTED_LOCATION ${PX4_BUILD_DIR}/src/lib/lat_lon_alt/liblat_lon_alt.a)
    add_dependencies(px4_lat_lon_alt px4_external)

    add_library(px4_world_magnetic_model STATIC IMPORTED GLOBAL)
    set_target_properties(px4_world_magnetic_model PROPERTIES IMPORTED_LOCATION ${PX4_BUILD_DIR}/src/lib/world_magnetic_model/libworld_magnetic_model.a)
    add_dependencies(px4_world_magnetic_model px4_external)

    add_library(px4_mode_util STATIC IMPORTED GLOBAL)
    set_target_properties(px4_mode_util PROPERTIES IMPORTED_LOCATION ${PX4_BUILD_DIR}/src/modules/commander/ModeUtil/libmode_util.a)
    add_dependencies(px4_mode_util px4_external)

    add_library(px4_bias_estimator STATIC IMPORTED GLOBAL)
    set_target_properties(px4_bias_estimator PROPERTIES IMPORTED_LOCATION ${PX4_BUILD_DIR}/src/modules/ekf2/EKF/bias_estimator/libbias_estimator.a)
    add_dependencies(px4_bias_estimator px4_external)

    add_library(px4_yaw_estimator STATIC IMPORTED GLOBAL)
    set_target_properties(px4_yaw_estimator PROPERTIES IMPORTED_LOCATION ${PX4_BUILD_DIR}/src/modules/ekf2/EKF/yaw_estimator/libyaw_estimator.a)
    add_dependencies(px4_yaw_estimator px4_external)

    add_library(px4_version STATIC IMPORTED GLOBAL)
    set_target_properties(px4_version PROPERTIES IMPORTED_LOCATION ${PX4_BUILD_DIR}/src/lib/version/libversion.a)
    add_dependencies(px4_version px4_external)

    add_library(px4_board_drivers STATIC IMPORTED GLOBAL)
    set_target_properties(px4_board_drivers PROPERTIES IMPORTED_LOCATION ${PX4_BUILD_DIR}/boards/renesas/rzv/src/libdrivers_board.a)
    add_dependencies(px4_board_drivers px4_external)

    add_library(px4_rc STATIC IMPORTED GLOBAL)
    set_target_properties(px4_rc PROPERTIES IMPORTED_LOCATION ${PX4_BUILD_DIR}/src/lib/rc/librc.a)
    add_dependencies(px4_rc px4_lib)

    add_library(px4_device STATIC IMPORTED GLOBAL)
    set_target_properties(px4_device PROPERTIES IMPORTED_LOCATION ${PX4_BUILD_DIR}/src/lib/drivers/device/libdrivers__device.a)
    add_dependencies(px4_device px4_lib)

    add_library(px4_layer STATIC IMPORTED GLOBAL)
    set_target_properties(px4_layer PROPERTIES IMPORTED_LOCATION ${PX4_BUILD_DIR}/platforms/posix/src/px4/common/libpx4_layer.a)
    add_dependencies(px4_layer px4_lib)

    add_library(px4_microxrcedds_client STATIC IMPORTED GLOBAL)
    set_target_properties(px4_microxrcedds_client PROPERTIES IMPORTED_LOCATION ${PX4_BUILD_DIR}/src/modules/uxrce_dds_client/lib/libmicroxrcedds_client.a)
    add_dependencies(px4_microxrcedds_client px4_external)

    add_library(px4_microcdr STATIC IMPORTED GLOBAL)
    set_target_properties(px4_microcdr PROPERTIES IMPORTED_LOCATION ${PX4_BUILD_DIR}/src/modules/uxrce_dds_client/lib/libmicrocdr.a)
    add_dependencies(px4_microcdr px4_external)

    set(_px4_link_libs
        px4_lib
        px4_uorb_msgs
        px4_perf
        px4_tinybson
        px4_geo
        px4_modules_ekf2
        px4_output_predictor
        px4_lat_lon_alt
        px4_world_magnetic_model
        px4_mode_util
        px4_bias_estimator
        px4_version
        px4_board_drivers
        px4_rc
        px4_device
        px4_layer
        px4_microxrcedds_client
        px4_microcdr
    )

    if(TARGET px4_yaw_estimator)
        list(APPEND _px4_link_libs px4_yaw_estimator)
    endif()

    add_library(px4_rzv INTERFACE)
    add_dependencies(px4_rzv ${_px4_link_libs})
    target_link_libraries(px4_rzv INTERFACE ${_px4_link_libs})
    target_include_directories(px4_rzv INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/px4
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/src
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/src/include
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/src/modules
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/src/lib
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/platforms
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/platforms/common/include
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/platforms/posix/src/px4/common
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/platforms/posix/src/px4/common/include
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/platforms/posix/src/px4/generic/generic/include
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/platforms/posix/include
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/board
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/hal
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/services
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/shim
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/shim/px4
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/shim/freertos
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
        ${CMAKE_CURRENT_SOURCE_DIR}/src/rpc
        ${PX4_BUILD_DIR}
        ${PX4_BUILD_DIR}/src
        ${PX4_BUILD_DIR}/generated_params
        ${PX4_BUILD_DIR}/src/lib/parameters
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/aws/FreeRTOS/FreeRTOS/Source/include
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/aws/FreeRTOS/FreeRTOS-Plus/Source/FreeRTOS-Plus-Common/include
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/fsp/inc
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/fsp/inc/api
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/fsp/inc/instances
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv_cfg/aws
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv_cfg/fsp_cfg
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv_cfg/fsp_cfg/bsp
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv_gen
    )
    target_compile_definitions(px4_rzv INTERFACE
        __PX4_FREERTOS=1
        __PX4_POSIX=1
        _RENESAS_RZV_
        _RZV_CORE=CR8_0
        _RZV_ORDINAL=2
        CONFIG_ORB_COMMUNICATOR
        PX4_ENABLE_STACK_MONITOR=$<BOOL:${PX4_ENABLE_STACK_MONITOR}>
        CONFIG_RZV_MEM_TELEMETRY=$<BOOL:${CONFIG_RZV_MEM_TELEMETRY}>
    )

    add_subdirectory(px4/boards/renesas/rzv/src/hal/drivers)

    target_link_libraries(px4_rzv INTERFACE drivers_rzv_fsp)

    # Gather FreeRTOS/BSP application sources
    file(GLOB_RECURSE RZV2H_PX4_FREERTOS_C_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
    )

    set(FREERTOS_POSIX_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src/freertos_posix)

    set(RZV2H_PX4_FREERTOS_C_SOURCES
        src/runtime/hal_entry.c
        px4/boards/renesas/rzv/src/shim/px4/cache_shim.c
        src/runtime/main_task_entry.c
        src/runtime/mpu_region_table.c
        src/platform/pd_axi_on.c
        src/platform/platform_info.c
        px4/boards/renesas/rzv/src/shim/px4/posix_ioctl_stub.c
        px4/boards/renesas/rzv/src/shim/px4/posix_fs_stubs.c
        src/platform/rsc_table.c
        src/runtime/rzv2_rproc.c
        px4/boards/renesas/rzv/src/storage/include/sys_stub.c
        src/runtime/sys_init.c
        src/runtime/rzv_runtime_stats.c
        # syscalls are built into a dedicated OBJECT library so we can force
        # they appear early on the link command line (avoid pulling libc stubs
        # that emit warnings). The file is removed from the usual sources and
        # compiled separately below as an OBJECT target.
        rzv_gen/common_data.c
        rzv_gen/hal_data.c
        rzv_gen/main.c
        rzv_gen/main_task.c
        rzv_gen/pin_data.c
        rzv_gen/vector_data.c
        rzv/aws/FreeRTOS/FreeRTOS/Source/event_groups.c
        rzv/aws/FreeRTOS/FreeRTOS/Source/list.c
        rzv/aws/FreeRTOS/FreeRTOS/Source/queue.c
        rzv/aws/FreeRTOS/FreeRTOS/Source/stream_buffer.c
        rzv/aws/FreeRTOS/FreeRTOS/Source/tasks.c
        rzv/aws/FreeRTOS/FreeRTOS/Source/timers.c
        rzv/aws/FreeRTOS/FreeRTOS/Source/portable/MemMang/heap_4.c
        rzv/fsp/src/rm_freertos_port/cr/port.c
        rzv/fsp/src/r_ioport/r_ioport.c
        rzv/fsp/src/r_mhu_b_ns/r_mhu_b_ns.c
        rzv/fsp/src/r_sci_b_uart/r_sci_b_uart.c
        rzv/fsp/src/r_gpt/r_gpt.c
        rzv/fsp/src/r_gtm/r_gtm.c
        rzv/fsp/src/r_spi_b/r_spi_b.c
        rzv/fsp/src/r_sci_b_i2c/r_sci_b_i2c.c
        rzv/fsp/src/r_dmac_b/r_dmac_b.c
        rzv/fsp/src/bsp/mcu/all/bsp_address_convert.c
        rzv/fsp/src/bsp/mcu/all/bsp_clocks.c
        rzv/fsp/src/bsp/mcu/all/bsp_common.c
        rzv/fsp/src/bsp/mcu/all/bsp_delay.c
        rzv/fsp/src/bsp/mcu/all/bsp_guard.c
        rzv/fsp/src/bsp/mcu/all/bsp_group_irq.c
        rzv/fsp/src/bsp/mcu/all/bsp_io.c
        rzv/fsp/src/bsp/mcu/all/bsp_sbrk.c
        rzv/fsp/src/bsp/mcu/all/bsp_select_irq.c
        rzv/fsp/src/bsp/mcu/all/cr/bsp_irq.c
        rzv/fsp/src/bsp/mcu/all/cr/bsp_mpu.c
        rzv/fsp/src/bsp/mcu/all/cr/bsp_security.c
        rzv/fsp/src/bsp/cmsis/Device/RENESAS/Source/cr/dummy.c
        rzv/fsp/src/bsp/cmsis/Device/RENESAS/Source/cr/system.c
        rzv/board/rzv2h_evk/board_init.c
        rzv/board/rzv2h_evk/board_leds.c
        rzv/linaro/open-amp/lib/remoteproc/elf_loader.c
        rzv/linaro/open-amp/lib/remoteproc/remoteproc.c
        rzv/linaro/open-amp/lib/remoteproc/remoteproc_virtio.c
        rzv/linaro/open-amp/lib/remoteproc/rsc_table_parser.c
        rzv/linaro/open-amp/lib/rpmsg/rpmsg.c
        rzv/linaro/open-amp/lib/rpmsg/rpmsg_virtio.c
        rzv/linaro/open-amp/lib/virtio/virtio.c
        rzv/linaro/open-amp/lib/virtio/virtqueue.c
        rzv/linaro/libmetal/lib/device.c
        rzv/linaro/libmetal/lib/dma.c
        rzv/linaro/libmetal/lib/init.c
        rzv/linaro/libmetal/lib/io.c
        rzv/linaro/libmetal/lib/log.c
        rzv/linaro/libmetal/lib/shmem.c
        rzv/linaro/libmetal/lib/system/freertos/condition.c
        rzv/linaro/libmetal/lib/system/freertos/device.c
        rzv/linaro/libmetal/lib/system/freertos/init.c
        rzv/linaro/libmetal/lib/system/freertos/io.c
        rzv/linaro/libmetal/lib/system/freertos/irq.c
        rzv/linaro/libmetal/lib/system/freertos/rzv2/sys.c
        rzv/linaro/libmetal/lib/system/freertos/shmem.c
        rzv/linaro/libmetal/lib/system/freertos/time.c
        rzv/linaro/libmetal/lib/version.c
        src/SEGGER_RTT/RTT/SEGGER_RTT.c
        src/SEGGER_RTT/RTT/SEGGER_RTT_printf.c
        ${FREERTOS_POSIX_ROOT}/source/FreeRTOS_POSIX_clock.c
        ${FREERTOS_POSIX_ROOT}/source/FreeRTOS_POSIX_mqueue.c
        ${FREERTOS_POSIX_ROOT}/source/FreeRTOS_POSIX_pthread.c
        ${FREERTOS_POSIX_ROOT}/source/FreeRTOS_POSIX_pthread_barrier.c
        ${FREERTOS_POSIX_ROOT}/source/FreeRTOS_POSIX_pthread_cond.c
        ${FREERTOS_POSIX_ROOT}/source/FreeRTOS_POSIX_pthread_mutex.c
        ${FREERTOS_POSIX_ROOT}/source/FreeRTOS_POSIX_sched.c
        ${FREERTOS_POSIX_ROOT}/source/FreeRTOS_POSIX_semaphore.c
        ${FREERTOS_POSIX_ROOT}/source/FreeRTOS_POSIX_timer.c
        ${FREERTOS_POSIX_ROOT}/source/FreeRTOS_POSIX_utils.c
    )

    # RPC client and socket stub sources (CA55 is always enabled)
    list(APPEND RZV2H_PX4_FREERTOS_C_SOURCES
        src/rpc/openamp_rpc_client.c
        src/rpc/remote_storage_rpc.c
        px4/boards/renesas/rzv/src/shim/px4/posix_socket_stub.c
    )

    set(WARNING_SUPPRESS_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SEGGER_RTT/RTT/SEGGER_RTT_printf.c
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/linaro/libmetal/lib/system/freertos/rzv2/sys.c
        ${FREERTOS_POSIX_ROOT}/source/FreeRTOS_POSIX_clock.c
        ${FREERTOS_POSIX_ROOT}/source/FreeRTOS_POSIX_mqueue.c
        ${FREERTOS_POSIX_ROOT}/source/FreeRTOS_POSIX_pthread.c
        ${FREERTOS_POSIX_ROOT}/source/FreeRTOS_POSIX_pthread_barrier.c
        ${FREERTOS_POSIX_ROOT}/source/FreeRTOS_POSIX_pthread_cond.c
        ${FREERTOS_POSIX_ROOT}/source/FreeRTOS_POSIX_pthread_mutex.c
        ${FREERTOS_POSIX_ROOT}/source/FreeRTOS_POSIX_sched.c
        ${FREERTOS_POSIX_ROOT}/source/FreeRTOS_POSIX_semaphore.c
        ${FREERTOS_POSIX_ROOT}/source/FreeRTOS_POSIX_timer.c
        ${FREERTOS_POSIX_ROOT}/source/FreeRTOS_POSIX_utils.c
    )

    set_source_files_properties(${WARNING_SUPPRESS_SOURCES}
        PROPERTIES COMPILE_OPTIONS "-Wno-unused-function;-Wno-unused-parameter;-Wno-cast-function-type;-Wno-address"
    )

    # rzv_gen/hal_data.c is auto-generated by FSP and always contains
    # vPortConfigureTimerForRunTimeStatus() and ulGetRunTimeCounterValue().
    # The improved versions live in src/runtime/rzv_runtime_stats.c.
    # Rename the FSP copies at compile-time so they don't conflict.
    set_source_files_properties(rzv_gen/hal_data.c PROPERTIES
        COMPILE_DEFINITIONS
        "vPortConfigureTimerForRunTimeStatus=vPortConfigureTimerForRunTimeStatus_fsp_unused;ulGetRunTimeCounterValue=ulGetRunTimeCounterValue_fsp_unused"
    )

    # Build syscalls as an OBJECT library so we can inject the object file(s)
    # early on the link line. This ensures our implementations satisfy
    # libc/newlib unresolved symbols without forcing the linker to pull
    # warning-emitting archive members.
    add_library(rzv2h_px4_freertos_syscalls OBJECT src/runtime/syscalls.c)
    target_include_directories(rzv2h_px4_freertos_syscalls PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv_cfg
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv_cfg/fsp_cfg
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv_cfg/fsp_cfg/bsp
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/aws/FreeRTOS/FreeRTOS/Source/include
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/aws/FreeRTOS/FreeRTOS-Plus/Source/FreeRTOS-Plus-Common/include
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/fsp/inc
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/fsp/inc/api
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/fsp/inc/instances
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/fsp/src/bsp/mcu/all
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/fsp/src/bsp/mcu/rzv2h
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/fsp/src/rm_freertos_port
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/fsp/src/rm_freertos_port/cr
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv_gen
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/board
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/hal
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/services
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/shim
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/shim/px4
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/shim/freertos
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/hal/drivers
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/hal/drivers/rzv_fsp/include
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/storage/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
        ${CMAKE_CURRENT_SOURCE_DIR}/src/rpc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/tools
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SEGGER_RTT/RTT
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SEGGER_RTT/Config
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/linaro/open-amp/lib/include
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/linaro/libmetal/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/freertos_posix/portable/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/freertos_posix/portable
        ${FREERTOS_POSIX_ROOT}/include
        ${FREERTOS_POSIX_ROOT}/include/portable
        ${FREERTOS_POSIX_ROOT}/include/FreeRTOS_POSIX
        ${FREERTOS_POSIX_ROOT}/include/private
    )
    # Ensure px4 headers / generated configuration are available when compiling
    # syscalls. Linking against the INTERFACE target px4_rzv will propagate
    # include directories and compile definitions.
    target_link_libraries(rzv2h_px4_freertos_syscalls PRIVATE px4_rzv)
    add_dependencies(rzv2h_px4_freertos_syscalls px4_lib)

    if(CONFIG_SYSCALL_DEBUG_VERBOSE)
        target_compile_definitions(rzv2h_px4_freertos_syscalls PRIVATE CONFIG_SYSCALL_DEBUG_VERBOSE)
    endif()

    # Project-specific FreeRTOS overrides: force-include before FreeRTOSConfig.h
    target_compile_options(rzv2h_px4_freertos_syscalls PRIVATE
        "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/freertos_posix/FreeRTOSCustomConfig.h"
    )

    set(RZV2H_PX4_FREERTOS_CPP_SOURCES
        px4/boards/renesas/rzv/src/shim/px4/px4_startup_shim.cpp
        px4/boards/renesas/rzv/src/shim/px4/posix_shim.cpp
        px4/boards/renesas/rzv/src/shim/px4/posix_shim_cpp.cpp
        px4/boards/renesas/rzv/src/shim/px4/px4_daemon_shim.cpp
        px4/boards/renesas/rzv/src/shim/px4/posix_termios_stubs.cpp
        px4/boards/renesas/rzv/src/hal/drivers/rzv_fsp/serial_impl_stub.cpp
        px4/boards/renesas/rzv/src/shim/px4/time_port_shim.cpp
        px4/boards/renesas/rzv/src/shim/px4/time_port_board.cpp
        px4/boards/renesas/rzv/src/shim/px4/time_shim.cpp
    )

    # Ensure incremental build detects changes to .incbin embedded files
    # The .cmds file is embedded via inline assembly, which CMake cannot track automatically
    set_source_files_properties(
        px4/boards/renesas/rzv/src/shim/px4/px4_startup_shim.cpp
        PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/init/rc.board_defaults.cmds;${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/init/rcS"
    )

    set(RZV2H_PX4_FREERTOS_ASM_SOURCES
        rzv/fsp/src/bsp/mcu/all/cr/bsp_irq_asm.asm
        rzv/fsp/src/bsp/mcu/all/cr/bsp_mpu_asm.asm
        rzv/fsp/src/rm_freertos_port/cr/portASM.asm
        rzv/fsp/src/bsp/cmsis/Device/RENESAS/Source/cr/startup.asm
    )

    set_source_files_properties(${RZV2H_PX4_FREERTOS_ASM_SOURCES} PROPERTIES
        LANGUAGE ASM
        COMPILE_FLAGS "-x assembler-with-cpp -mcpu=${ARM_CPU} -mthumb -mfpu=${ARM_FPU} -mfloat-abi=${ARM_FLOAT_ABI}"
    )

    # memory_regions.ld is generated by e2studio directly into the build
    # directory (Debug/). It must exist before linking; compile.sh preserves
    # it across clean/rebuild. If missing, generate the project with e2studio.
    if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/memory_regions.ld)
        message(FATAL_ERROR
            "memory_regions.ld not found in build directory.\n"
            "Generate the project with e2studio first so it creates "
            "${CMAKE_CURRENT_BINARY_DIR}/memory_regions.ld, "
            "then re-run compile.sh.")
    endif()

    add_executable(rzv2h_px4_freertos.elf
        ${RZV2H_PX4_FREERTOS_C_SOURCES}
        ${RZV2H_PX4_FREERTOS_CPP_SOURCES}
        ${RZV2H_PX4_FREERTOS_ASM_SOURCES}
    )

    if(_ccache_program)
        set_property(TARGET rzv2h_px4_freertos.elf PROPERTY C_COMPILER_LAUNCHER ${_ccache_launcher})
        set_property(TARGET rzv2h_px4_freertos.elf PROPERTY CXX_COMPILER_LAUNCHER ${_ccache_launcher})
    endif()

    target_include_directories(rzv2h_px4_freertos.elf PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
        ${CMAKE_CURRENT_SOURCE_DIR}/src/rpc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/tools
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/board
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/hal
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/services
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/shim
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/shim/px4
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/shim/freertos
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/hal/drivers
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/hal/drivers/rzv_fsp/include
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv_cfg
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv_cfg/fsp_cfg
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv_cfg/fsp_cfg/bsp
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv_cfg/aws
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/aws/FreeRTOS/FreeRTOS/Source/include
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/aws/FreeRTOS/FreeRTOS-Plus/Source/FreeRTOS-Plus-Common/include
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/fsp/inc
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/fsp/inc/api
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/fsp/inc/instances
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/fsp/src/bsp/mcu/all
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/fsp/src/bsp/mcu/rzv2h
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/fsp/src/rm_freertos_port
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/fsp/src/rm_freertos_port/cr
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv_gen
        ${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/src/storage/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SEGGER_RTT/RTT
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SEGGER_RTT/Config
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/linaro/open-amp/lib/include
        ${CMAKE_CURRENT_SOURCE_DIR}/rzv/linaro/libmetal/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/freertos_posix/portable/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/freertos_posix/portable
        ${FREERTOS_POSIX_ROOT}/include
        ${FREERTOS_POSIX_ROOT}/include/portable
        ${FREERTOS_POSIX_ROOT}/include/FreeRTOS_POSIX
        ${FREERTOS_POSIX_ROOT}/include/private
    )

    target_compile_definitions(rzv2h_px4_freertos.elf PRIVATE
        __PX4_FREERTOS=1
        __PX4_POSIX=1
        _RENESAS_RZV_
        _RZV_CORE=CR8_0
        _RZV_ORDINAL=2
        CONFIG_ORB_COMMUNICATOR
        METAL_INTERNAL=1
        configUSE_POSIX_ERRNO=1
        __LINUX_ERRNO_EXTENSIONS__=1
        _POSIX_C_SOURCE=200809L
        _XOPEN_SOURCE=700
        EAFNOSUPPORT=106
        EADDRINUSE=112
        EADDRNOTAVAIL=125
        EISCONN=127
        EALREADY=120
        EDESTADDRREQ=121
        ENETDOWN=115
        ENETUNREACH=114
        ENETRESET=126
        ENOBUFS=105
        ENOTCONN=128
        EOPNOTSUPP=95
        EPROTO=71
        EPROTONOSUPPORT=123
        EPROTOTYPE=107
        ECANCELED=140
        EOWNERDEAD=142
        ENOTRECOVERABLE=141
        EWOULDBLOCK=EAGAIN
        PX4_ENABLE_STACK_MONITOR=$<BOOL:${PX4_ENABLE_STACK_MONITOR}>
        CONFIG_RZV_MEM_TELEMETRY=$<BOOL:${CONFIG_RZV_MEM_TELEMETRY}>
        ENOTSUP=95
        EDOM=33
        ECONNABORTED=103
        ECONNREFUSED=111
        ECONNRESET=104
        EHOSTUNREACH=113
        ECHILD=10
        ENOLCK=37
        ENOMSG=42
        ENOPROTOOPT=92
        ENOTSOCK=88
        EINPROGRESS=115
        ELOOP=40
        RC_BOARD_DEFAULTS_CMDS_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/init/rc.board_defaults.cmds\"
        RC_BOARD_RCS_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/px4/boards/renesas/rzv/init/rcS\"
    )

    # Link our syscalls object(s) before the PX4 libraries so that the
    # linker's symbol resolution prefers our definitions and avoids pulling
    # libc archive members that include .warning directives.
    target_link_libraries(rzv2h_px4_freertos.elf PRIVATE
        $<TARGET_OBJECTS:rzv2h_px4_freertos_syscalls>
        px4_rzv
    )

    target_link_options(rzv2h_px4_freertos.elf PRIVATE
        -Wl,-L${CMAKE_CURRENT_BINARY_DIR}
        -Wl,-L${CMAKE_CURRENT_SOURCE_DIR}/script
        -T${CMAKE_CURRENT_SOURCE_DIR}/script/rzv2h_evk_cr.ld
        -mcpu=${ARM_CPU}
        -mthumb
        -mfpu=${ARM_FPU}
        -mfloat-abi=${ARM_FLOAT_ABI}
        -Wl,--gc-sections
        -Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/rzv2h_px4_freertos.map
    )

    target_compile_options(rzv2h_px4_freertos.elf PRIVATE
        -fomit-frame-pointer
        -fstack-usage
        # Project-specific FreeRTOS overrides: force-include before FreeRTOSConfig.h
        "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/freertos_posix/FreeRTOSCustomConfig.h"
    )

    set_target_properties(rzv2h_px4_freertos.elf PROPERTIES
        OUTPUT_NAME "rzv2h_px4_freertos"
        SUFFIX ".elf"
    )

    add_dependencies(rzv2h_px4_freertos.elf px4_lib)

    add_custom_command(TARGET rzv2h_px4_freertos.elf POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:rzv2h_px4_freertos.elf> ${CMAKE_BINARY_DIR}/rzv2h_px4_freertos.elf
        COMMAND ${CMAKE_COMMAND} -E env BuildArtifactFileName=rzv2h_px4_freertos.elf bash ${CMAKE_SOURCE_DIR}/script/postbuild.sh
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running postbuild script"
    )

    add_custom_target(memory_layout_check ALL
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/memory/check_sections.py
            --elf $<TARGET_FILE:rzv2h_px4_freertos.elf>
        DEPENDS rzv2h_px4_freertos.elf
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Validating memory layout (check_sections.py)"
        VERBATIM
    )
else()
    message(WARNING "PX4 directory not found at ${PX4_DIR} or missing CMakeLists.txt. px4_lib will not be generated.")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
