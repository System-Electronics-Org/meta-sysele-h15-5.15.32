project(
  'sysele-dsi-ai-isp',
  'cpp',
  version : '1.0.0',
  default_options : [
    'cpp_std=c++17',
    'buildtype=release',
    'warning_level=2',
  ],
)

gst_dep = dependency('gstreamer-1.0')
gst_app_dep = dependency('gstreamer-app-1.0')
gst_video_dep = dependency('gstreamer-video-1.0')
opencv_dep = dependency('opencv4')
hailo_medialib_dep = dependency('hailo-medialib')
thread_dep = dependency('threads')

executable(
  'dsi_ai_isp_app',
  'main.cpp',
  dependencies : [
    gst_dep,
    gst_app_dep,
    gst_video_dep,
    opencv_dep,
    hailo_medialib_dep,
    thread_dep,
  ],
  install : true,
  install_dir : get_option('install_dir'),
)
