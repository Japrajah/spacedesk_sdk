add_executable(sd_send_keyboard main.cpp)
sd_sample(sd_send_keyboard)
target_link_libraries(sd_send_keyboard PRIVATE user32)
