cmake_minimum_required(VERSION 3.28.3)
project(ZeroRingCloud)
set(CMAKE_CXX_STANDARD 23)
add_executable(server backend/server.cpp backend/db_manager.cpp)