# Drogon's Filesystem finder restores CMAKE_CXX_STANDARD to its previous value.
# CMake 4 rejects an empty restored value, so provide a valid value while finding Drogon.
set(CMAKE_CXX_STANDARD 20)
find_package(Drogon CONFIG REQUIRED)
unset(CMAKE_CXX_STANDARD)
find_package(PkgConfig REQUIRED)
pkg_check_modules(Sodium REQUIRED IMPORTED_TARGET libsodium)

add_library(
  algorithm-trainer-core
  src/auth-service.cpp
  src/judge.cpp
  src/mock-judge.cpp
  src/nsjail-python-executor.cpp
  src/problem.cpp
  src/problem-judge.cpp
  src/problem-service.cpp
  src/sqlite-problem-repository.cpp
  src/sqlite-submission-repository.cpp
  src/submission.cpp
  src/submission-record.cpp
  src/submission-service.cpp
  src/submission-worker.cpp
)

foreach(runtime_variable IN ITEMS
        ALGORITHM_TRAINER_NSJAIL_PATH
        ALGORITHM_TRAINER_PYTHON_PATH
        ALGORITHM_TRAINER_PYTHON_RUNTIME_CLOSURE
        ALGORITHM_TRAINER_CPP_COMPILER_PATH
        ALGORITHM_TRAINER_CPP_RUNTIME_CLOSURE)
  if(NOT DEFINED ENV{${runtime_variable}})
    message(FATAL_ERROR "${runtime_variable} is missing; configure CMake inside nix develop")
  endif()
endforeach()

target_compile_definitions(
  algorithm-trainer-core
  PRIVATE
    ALGORITHM_TRAINER_NSJAIL_PATH="$ENV{ALGORITHM_TRAINER_NSJAIL_PATH}"
    ALGORITHM_TRAINER_PYTHON_PATH="$ENV{ALGORITHM_TRAINER_PYTHON_PATH}"
    ALGORITHM_TRAINER_PYTHON_RUNTIME_CLOSURE="$ENV{ALGORITHM_TRAINER_PYTHON_RUNTIME_CLOSURE}"
    ALGORITHM_TRAINER_CPP_COMPILER_PATH="$ENV{ALGORITHM_TRAINER_CPP_COMPILER_PATH}"
    ALGORITHM_TRAINER_CPP_RUNTIME_CLOSURE="$ENV{ALGORITHM_TRAINER_CPP_RUNTIME_CLOSURE}"
    ALGORITHM_TRAINER_MIGRATIONS_DIR="${PROJECT_SOURCE_DIR}/migrations"
)
target_compile_features(algorithm-trainer-core PUBLIC cxx_std_20)
target_include_directories(algorithm-trainer-core PUBLIC include)
target_link_libraries(algorithm-trainer-core PUBLIC Drogon::Drogon PkgConfig::Sodium SQLite3_lib)

add_executable(algorithm-trainer-backend src/main.cpp)
target_compile_features(algorithm-trainer-backend PRIVATE cxx_std_20)
target_link_libraries(algorithm-trainer-backend PRIVATE algorithm-trainer-core)

if(BUILD_TESTING)
  find_package(Catch2 3 CONFIG REQUIRED)

  add_executable(
    algorithm-trainer-tests
    tests/a-plus-b-judge-test.cpp
    tests/auth-service-test.cpp
    tests/judge-test.cpp
    tests/problem-test.cpp
    tests/sqlite-problem-repository-test.cpp
    tests/smoke-test.cpp
    tests/sqlite-submission-repository-test.cpp
    tests/submission-service-test.cpp
    tests/submission-test.cpp
    tests/submission-worker-test.cpp
  )
  target_compile_features(algorithm-trainer-tests PRIVATE cxx_std_20)
  target_link_libraries(
    algorithm-trainer-tests
    PRIVATE
      algorithm-trainer-core
      Catch2::Catch2WithMain
  )

  include(Catch)
  catch_discover_tests(algorithm-trainer-tests)

  add_executable(
    algorithm-trainer-sandbox-tests
    tests/nsjail-python-executor-test.cpp
  )
  target_compile_features(algorithm-trainer-sandbox-tests PRIVATE cxx_std_20)
  target_link_libraries(
    algorithm-trainer-sandbox-tests
    PRIVATE
      algorithm-trainer-core
      Catch2::Catch2WithMain
  )
  catch_discover_tests(
    algorithm-trainer-sandbox-tests
    TEST_PREFIX "sandbox: "
    PROPERTIES LABELS sandbox
  )
endif()
