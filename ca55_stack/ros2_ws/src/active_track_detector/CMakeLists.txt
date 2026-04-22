cmake_minimum_required(VERSION 3.8)
project(active_track_detector)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Board's spdlog uses system fmt (no bundled headers)
add_compile_definitions(SPDLOG_FMT_EXTERNAL)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── NEON FP16 flags (ARM aarch64, required for yolo post-processing) ──
include(src/ai/neon_fp16.cmake OPTIONAL)

# ── DRP-AI TVM prebuilt location ──────────────────────────────────────
# Set via: -DCA55_PREBUILT_DIR=<path> or env CA55_PREBUILT_DIR
if(NOT CA55_PREBUILT_DIR)
  set(CA55_PREBUILT_DIR "$ENV{CA55_PREBUILT_DIR}")
endif()
if(NOT CA55_PREBUILT_DIR)
  # Fallback: look relative to this package (inside ca55_stack/third_party_bin)
  get_filename_component(WS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
  set(CA55_PREBUILT_DIR "${WS_ROOT}/third_party_bin")
endif()
message(STATUS "CA55_PREBUILT_DIR = ${CA55_PREBUILT_DIR}")

set(DRP_LIB_DIR  "${CA55_PREBUILT_DIR}/lib/aarch64")
set(DRP_INC_DIR  "${CA55_PREBUILT_DIR}/include")

# ── ROS2 dependencies ─────────────────────────────────────────────────
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(active_track_msgs REQUIRED)

# detector_node (requires DRP-AI TVM SDK — cross-compile only, skip on board)
# add_executable(detector_node ...) — omitted; use detection_bridge_node instead

# ── Detection Bridge: UDP → ROS2 (no DRP-AI dependency) ──────────────
add_executable(detection_bridge_node
  src/detection_bridge_node.cpp
)
ament_target_dependencies(detection_bridge_node
  rclcpp
  std_msgs
  active_track_msgs
)
install(TARGETS detection_bridge_node
  DESTINATION lib/${PROJECT_NAME}
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/launch")
  install(DIRECTORY launch/ DESTINATION share/${PROJECT_NAME}/launch)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/config")
  install(DIRECTORY config/ DESTINATION share/${PROJECT_NAME}/config)
endif()

ament_package()
