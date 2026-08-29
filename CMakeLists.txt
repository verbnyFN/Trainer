cmake_minimum_required(VERSION 3.25)

project(
  algorithm-trainer
  VERSION 0.1.0
  LANGUAGES CXX
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

add_subdirectory(backend)

if(BUILD_TESTING)
  find_program(PNPM_EXECUTABLE pnpm REQUIRED)
  add_test(NAME "frontend: application behavior"
           COMMAND "${PNPM_EXECUTABLE}" --dir "${PROJECT_SOURCE_DIR}/frontend" test)
  set_tests_properties("frontend: application behavior" PROPERTIES LABELS frontend TIMEOUT 30)
endif()
