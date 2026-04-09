cmake_minimum_required(VERSION 3.20)
project(MiMoCA LANGUAGES CXX)

add_executable(mimoca src/main.cpp)
target_compile_features(mimoca PRIVATE cxx_std_20)

find_package(OpenCV QUIET COMPONENTS core videoio)
if(OpenCV_FOUND)
  target_compile_definitions(mimoca PRIVATE MIMOCA_HAS_OPENCV=1)
  target_link_libraries(mimoca PRIVATE ${OpenCV_LIBS})
endif()

if(MSVC)
  target_compile_options(mimoca PRIVATE /W4 /permissive-)
  target_link_libraries(mimoca PRIVATE sapi ole32)
else()
  target_compile_options(mimoca PRIVATE -Wall -Wextra -Wpedantic)
endif()
