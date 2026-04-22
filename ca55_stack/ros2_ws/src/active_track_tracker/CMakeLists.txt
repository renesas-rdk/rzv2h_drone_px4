cmake_minimum_required(VERSION 3.8)
project(active_track_tracker)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(active_track_msgs REQUIRED)

add_executable(tracker_node src/tracker_node.cpp)

target_include_directories(tracker_node PRIVATE src)

ament_target_dependencies(tracker_node
  rclcpp std_msgs active_track_msgs)

install(TARGETS tracker_node DESTINATION lib/${PROJECT_NAME})

ament_package()
