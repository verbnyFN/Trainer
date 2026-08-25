cmake_minimum_required(VERSION 3.25)

project(
  algorithm-trainer
  VERSION 0.1.0
  LANGUAGES CXX
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

add_subdirectory(backend)
