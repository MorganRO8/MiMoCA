cmake_minimum_required(VERSION 3.20)
project(MiMoCA LANGUAGES CXX)

add_executable(mimoca src/main.cpp)
target_compile_features(mimoca PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(mimoca PRIVATE /W4 /permissive-)
else()
  target_compile_options(mimoca PRIVATE -Wall -Wextra -Wpedantic)
endif()
