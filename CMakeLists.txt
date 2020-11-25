cmake_minimum_required(VERSION 3.9)
project(xdg-autostart-launcher)

set(CMAKE_CXX_STANDARD 20)

add_executable(xdg-autostart-launcher main.cc)
install(TARGETS xdg-autostart-launcher DESTINATION bin)

