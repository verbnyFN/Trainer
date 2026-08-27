#include "algorithm-trainer/a-plus-b-judge.h"
#include "algorithm-trainer/executor.h"
#include "algorithm-trainer/judge.h"
#include "algorithm-trainer/nsjail-python-executor.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <variant>

namespace {

algorithm_trainer::ExecutionResult require_execution(algorithm_trainer::ExecutorResult result) {
  if (const auto *error = std::get_if<algorithm_trainer::ExecutorError>(&result)) {
    SKIP("NsJail is unavailable in this kernel/environment: " << error->message);
  }
  return std::get<algorithm_trainer::ExecutionResult>(std::move(result));
}

algorithm_trainer::ExecutionResult run_python(std::string source, std::string input = {}) {
  algorithm_trainer::NsJailPythonExecutor executor;
  return require_execution(executor.run({
      .language = "python",
      .source_code = std::move(source),
      .standard_input = std::move(input),
  }));
}

algorithm_trainer::ExecutionResult run_cpp(std::string source, std::string input = {}) {
  algorithm_trainer::NsJailPythonExecutor executor;
  return require_execution(executor.run({
      .language = "cpp",
      .source_code = std::move(source),
      .standard_input = std::move(input),
  }));
}

algorithm_trainer::JudgeResult judge(std::string source, std::string language = "python") {
  algorithm_trainer::NsJailPythonExecutor executor;
  algorithm_trainer::APlusBJudge judge{executor};
  return judge.run({
      .problem_id = "a-plus-b",
      .language = std::move(language),
      .source_code = std::move(source),
  });
}

algorithm_trainer::Verdict require_verdict(algorithm_trainer::JudgeResult result) {
  if (const auto *error = std::get_if<algorithm_trainer::JudgeError>(&result)) {
    SKIP("NsJail is unavailable in this kernel/environment: " << error->message);
  }
  return std::get<algorithm_trainer::Verdict>(result);
}

} // namespace

TEST_CASE("NsJail executes a correct A+B submission", "[sandbox]") {
  const auto verdict = require_verdict(judge("a, b = map(int, input().split())\nprint(a + b)\n"));
  CHECK(verdict == algorithm_trainer::Verdict::accepted);
}

TEST_CASE("NsJail compiles and executes a C++20 submission", "[sandbox][cpp]") {
  const auto result =
      run_cpp("#include <iostream>\nint main() { long long a, b; std::cin >> a >> b; "
              "std::cout << a + b << '\\n'; }\n",
              "2 3\n");

  CHECK_FALSE(result.timed_out);
  CHECK(result.exit_code == 0);
  CHECK(result.standard_output == "5\n");
}

TEST_CASE("NsJail judges a correct C++20 A+B solution", "[sandbox][cpp]") {
  const auto result = judge("#include <iostream>\nint main() { long long a, b; std::cin >> a >> b; "
                            "std::cout << a + b << '\\n'; }\n",
                            "cpp");

  REQUIRE(std::holds_alternative<algorithm_trainer::Verdict>(result));
  CHECK(std::get<algorithm_trainer::Verdict>(result) == algorithm_trainer::Verdict::accepted);
}

TEST_CASE("NsJail reports C++ compilation errors safely", "[sandbox][cpp]") {
  const auto result = run_cpp("int main( {\n");

  CHECK_FALSE(result.timed_out);
  CHECK(result.exit_code != 0);
  CHECK(result.error_type == "Compilation Error");
}

TEST_CASE("NsJail applies runtime limits to C++ submissions", "[sandbox][cpp]") {
  const auto result = run_cpp("int main() { volatile int value = 0; while (true) { ++value; } }\n");

  CHECK(result.timed_out);
}

TEST_CASE("NsJail output is judged normally", "[sandbox]") {
  const auto verdict = require_verdict(judge("print(999)\n"));
  CHECK(verdict == algorithm_trainer::Verdict::wrong_answer);
}

TEST_CASE("NsJail reports Python exceptions as non-zero exits", "[sandbox]") {
  const auto result = run_python("raise RuntimeError('boom')\n");
  CHECK_FALSE(result.timed_out);
  CHECK(result.exit_code != 0);
  CHECK(result.standard_error.find("RuntimeError") != std::string::npos);
}

TEST_CASE("NsJail reports Python syntax errors as non-zero exits", "[sandbox]") {
  const auto result = run_python("if:\n");
  CHECK_FALSE(result.timed_out);
  CHECK(result.exit_code != 0);
  CHECK(result.standard_error.find("SyntaxError") != std::string::npos);
}

TEST_CASE("NsJail terminates infinite execution", "[sandbox]") {
  const auto result = run_python("while True:\n    pass\n");
  CHECK(result.timed_out);
}

TEST_CASE("NsJail bounds combined stdout and stderr", "[sandbox]") {
  const auto result = run_python("while True:\n    print('x' * 10000)\n");
  CHECK_FALSE(result.timed_out);
  CHECK(result.exit_code != 0);
  CHECK(result.standard_output.size() + result.standard_error.size() <= 64 * 1024 + 32);
  CHECK(result.standard_error.find("Output limit exceeded") != std::string::npos);
}

TEST_CASE("NsJail hides the host filesystem", "[sandbox]") {
  const auto result = run_python(R"(
try:
    open('/etc/passwd').read()
    print('exposed')
except (FileNotFoundError, PermissionError):
    print('blocked')
)");
  CHECK(result.exit_code == 0);
  CHECK(result.standard_output == "blocked\n");
}

TEST_CASE("NsJail denies network socket creation", "[sandbox]") {
  const auto result = run_python(R"(
import socket
try:
    socket.socket()
    print('exposed')
except (OSError, PermissionError):
    print('blocked')
)");
  CHECK(result.exit_code == 0);
  CHECK(result.standard_output == "blocked\n");
}

TEST_CASE("NsJail denies child process creation", "[sandbox]") {
  const auto result = run_python(R"(
import os
try:
    os.fork()
    print('exposed')
except (OSError, PermissionError):
    print('blocked')
)");
  CHECK(result.exit_code == 0);
  CHECK(result.standard_output == "blocked\n");
}

TEST_CASE("NsJail bounds Python address space", "[sandbox]") {
  const auto result = run_python("data = bytearray(512 * 1024 * 1024)\n");
  CHECK_FALSE(result.timed_out);
  CHECK(result.exit_code != 0);
}

TEST_CASE("missing NsJail is an infrastructure error", "[sandbox]") {
  auto config = algorithm_trainer::NsJailPythonExecutor::default_config();
  config.nsjail_path = "/definitely-not-an-nsjail-binary";
  algorithm_trainer::NsJailPythonExecutor executor{std::move(config)};

  const auto result = executor.run({
      .language = "python",
      .source_code = "print('must never run')\n",
      .standard_input = {},
  });

  REQUIRE(std::holds_alternative<algorithm_trainer::ExecutorError>(result));
  CHECK(std::get<algorithm_trainer::ExecutorError>(result).message.find("could not be executed") !=
        std::string::npos);
}
