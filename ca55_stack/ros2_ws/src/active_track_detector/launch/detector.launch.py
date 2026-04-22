from launch import LaunchDescription
from launch_ros.actions import Node
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from ament_index_python.packages import get_package_share_directory
import os


def generate_launch_description():
    pkg = get_package_share_directory('active_track_detector')

    return LaunchDescription([
        DeclareLaunchArgument('device',        default_value='/dev/video0'),
        DeclareLaunchArgument('width',         default_value='640'),
        DeclareLaunchArgument('height',        default_value='480'),
        DeclareLaunchArgument('fps',           default_value='30'),
        DeclareLaunchArgument('model_dir',     default_value='/home/root/ai_models/yolov8n'),
        DeclareLaunchArgument('conf_threshold',default_value='0.25'),
        DeclareLaunchArgument('nms_iou',       default_value='0.45'),

        Node(
            package='active_track_detector',
            executable='detector_node',
            name='active_track_detector',
            output='screen',
            parameters=[{
                'device':         LaunchConfiguration('device'),
                'width':          LaunchConfiguration('width'),
                'height':         LaunchConfiguration('height'),
                'fps':            LaunchConfiguration('fps'),
                'model_dir':      LaunchConfiguration('model_dir'),
                'conf_threshold': LaunchConfiguration('conf_threshold'),
                'nms_iou':        LaunchConfiguration('nms_iou'),
            }],
        ),
    ])
