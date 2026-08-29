# Drogon's Filesystem finder restores CMAKE_CXX_STANDARD to its previous value.
# CMake 4 rejects an empty restored value, so provide a valid value while finding Drogon.
set(CMAKE_CXX_STANDARD 20)
find_package(Drogon CONFIG REQUIRED)
unset(CMAKE_CXX_STANDARD)
find_package(PkgConfig REQUIRED)
pkg_check_modules(Sodium REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(PostgreSQL REQUIRED IMPORTED_TARGET libpq)

add_library(
  algorithm-trainer-core
  src/auth-service.cpp
  src/isolated-process-executor.cpp
  src/judge.cpp
  src/mock-judge.cpp
  src/nsjail-python-executor.cpp
  src/problem.cpp
  src/problem-judge.cpp
  src/problem-service.cpp
  src/postgresql-auth-service.cpp
  src/postgresql-client.cpp
  src/postgresql-problem-repository.cpp
  src/postgresql-submission-repository.cpp
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
target_link_libraries(algorithm-trainer-core PUBLIC Drogon::Drogon PkgConfig::Sodium PkgConfig::PostgreSQL SQLite3_lib)

add_executable(algorithm-trainer-backend src/main.cpp)
target_compile_features(algorithm-trainer-backend PRIVATE cxx_std_20)
target_link_libraries(algorithm-trainer-backend PRIVATE algorithm-trainer-core)

add_executable(algorithm-trainer-judge-worker src/judge-worker-main.cpp)
target_compile_features(algorithm-trainer-judge-worker PRIVATE cxx_std_20)
target_link_libraries(algorithm-trainer-judge-worker PRIVATE algorithm-trainer-core)

target_compile_definitions(
  algorithm-trainer-backend
  PRIVATE ALGORITHM_TRAINER_JUDGE_WORKER_PATH="$<TARGET_FILE:algorithm-trainer-judge-worker>"
)
add_dependencies(algorithm-trainer-backend algorithm-trainer-judge-worker)

add_executable(algorithm-trainer-migrate-sqlite src/sqlite-to-postgresql.cpp)
target_compile_features(algorithm-trainer-migrate-sqlite PRIVATE cxx_std_20)
target_include_directories(algorithm-trainer-migrate-sqlite PRIVATE src)
target_link_libraries(algorithm-trainer-migrate-sqlite PRIVATE algorithm-trainer-core)

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
  target_compile_definitions(
    algorithm-trainer-sandbox-tests
    PRIVATE ALGORITHM_TRAINER_JUDGE_WORKER_PATH="$<TARGET_FILE:algorithm-trainer-judge-worker>"
  )
  add_dependencies(algorithm-trainer-sandbox-tests algorithm-trainer-judge-worker)
  target_link_libraries(
    algorithm-trainer-sandbox-tests
    PRIVATE
      algorithm-trainer-core
      Catch2::Catch2WithMain
  )
  add_test(NAME "sandbox: hardened execution suite"
           COMMAND sh "${PROJECT_SOURCE_DIR}/scripts/run-sandbox-tests"
                   "$<TARGET_FILE:algorithm-trainer-sandbox-tests>")
  set_tests_properties("sandbox: hardened execution suite" PROPERTIES
                       LABELS sandbox
                       TIMEOUT 120)

  add_executable(algorithm-trainer-postgresql-tests tests/postgresql-repository-test.cpp)
  target_compile_features(algorithm-trainer-postgresql-tests PRIVATE cxx_std_20)
  target_include_directories(algorithm-trainer-postgresql-tests PRIVATE src)
  target_link_libraries(algorithm-trainer-postgresql-tests PRIVATE algorithm-trainer-core Catch2::Catch2WithMain)
  catch_discover_tests(algorithm-trainer-postgresql-tests TEST_PREFIX "postgresql: " PROPERTIES LABELS postgresql)

  add_test(NAME "http: complete application submission flow"
           COMMAND sh "${PROJECT_SOURCE_DIR}/scripts/http-e2e-test")
  set_tests_properties("http: complete application submission flow" PROPERTIES
                       LABELS "http;e2e"
                       SKIP_RETURN_CODE 77
                       TIMEOUT 45)
endif()
