cmake_minimum_required(VERSION 3.16)
project(hagan_waves CXX)
set(CMAKE_CXX_STANDARD 20)
add_executable(scan_wave scan_wave.cpp)
target_include_directories(scan_wave PRIVATE ../..)

