"""
active_track.launch.py

Launches the full ActiveTrack pipeline:
  - detector_node   (separate process — owns DRP-AI, pinned to Core 2)
  - tracker_node    (composed in a single process with selector)
  - selector_node

Usage:
  ros2 launch active_track_bringup active_track.launch.py [target_class:=0] [policy:=highest_confidence]

To change target class at runtime:
  ros2 service call /active_track/set_target active_track_msgs/srv/SetTargetSelection \
    "{mode: 0, class_id: 2, policy: 'nearest_center'}"   # follow cars
  ros2 service call /active_track/set_target active_track_msgs/srv/SetTargetSelection \
    "{mode: 0, class_id: 0, policy: 'highest_confidence'}" # follow person
  ros2 service call /active_track/set_target active_track_msgs/srv/SetTargetSelection \
    "{mode: 2}"                                            # stop following
"""
from launch import LaunchDescription
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from ament_index_python.packages import get_package_share_directory
import os


def generate_launch_description():
    pkg = get_package_share_directory('active_track_bringup')
    config = os.path.join(pkg, 'config', 'active_track.yaml')

    return LaunchDescription([
        DeclareLaunchArgument('target_class', default_value='0',
                              description='COCO class to follow (0=person, 2=car, 15=cat)'),
        DeclareLaunchArgument('policy', default_value='highest_confidence',
                              description='highest_confidence | nearest_center | largest_bbox'),

        # Detector — separate process (owns DRP-AI hardware)
        Node(
            package='active_track_detector',
            executable='detector_node',
            name='active_track_detector',
            output='screen',
            parameters=[config],
            # Note: main() already sets CPU affinity to Core 2
        ),

        # Tracker — lightweight, can share process with selector
        Node(
            package='active_track_tracker',
            executable='tracker_node',
            name='active_track_tracker',
            output='screen',
            parameters=[config],
        ),

        # Selector
        Node(
            package='active_track_selector',
            executable='selector_node',
            name='active_track_selector',
            output='screen',
            parameters=[
                config,
                {
                    'default_class_id': LaunchConfiguration('target_class'),
                    'default_policy':   LaunchConfiguration('policy'),
                }
            ],
        ),
    ])
