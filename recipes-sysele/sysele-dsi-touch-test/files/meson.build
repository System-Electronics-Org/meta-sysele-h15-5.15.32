project(
  'sysele-dsi-touch-test',
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
opencv_dep = dependency('opencv4')
thread_dep = dependency('threads')

executable(
  'dsi_touch_test_app',
  'main.cpp',
  dependencies : [
    gst_dep,
    gst_app_dep,
    opencv_dep,
    thread_dep,
  ],
  install : true,
  install_dir : get_option('install_dir'),
)
