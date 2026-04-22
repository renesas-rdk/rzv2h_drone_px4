cmake_minimum_required(VERSION 3.8)
project(active_track_selector)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(active_track_msgs REQUIRED)

add_executable(selector_node src/selector_node.cpp)
ament_target_dependencies(selector_node rclcpp std_msgs active_track_msgs)
install(TARGETS selector_node DESTINATION lib/${PROJECT_NAME})
ament_package()
