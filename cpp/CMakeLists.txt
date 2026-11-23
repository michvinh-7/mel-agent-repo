cmake_minimum_required(VERSION 3.20)
project(mel_decide CXX)

set(CMAKE_CXX_STANDARD 20)  # <chrono> calendar types
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(mel_decide main.cpp decision.cpp timing.cpp validate.cpp)
target_compile_options(mel_decide PRIVATE -Wall -Wextra)
