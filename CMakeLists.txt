cmake_minimum_required(VERSION 3.9)
project(xdg-autostart-launcher)

find_package(pugixml REQUIRED)

set(CMAKE_CXX_STANDARD 20)

add_executable(xdg-autostart-launcher main.cc)
target_link_libraries(xdg-autostart-launcher pugixml)

