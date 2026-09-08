add_executable(sd_viewer WIN32 viewer.cpp settings_dialog.cpp connection_timers.cpp text_input.cpp)
sd_sample(sd_viewer)
target_link_libraries(sd_viewer PRIVATE user32 gdi32 shell32 comdlg32 winmm)
