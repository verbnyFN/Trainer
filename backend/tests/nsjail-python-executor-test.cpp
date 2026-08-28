#include "algorithm-trainer/executor.h"
#include "algorithm-trainer/judge.h"
#include "algorithm-trainer/nsjail-python-executor.h"
#include "algorithm-trainer/problem-judge.h"

#include <catch2/catch_test_macros.hpp>

#include "seed-problem-repository.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

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

algorithm_trainer::JudgeResult judge(std::string source, std::string language = "python",
                                     std::string problem_id = "a-plus-b") {
  algorithm_trainer::NsJailPythonExecutor executor;
  algorithm_trainer::ProblemJudge judge{executor, test_problem_service()};
  return judge.run({
      .problem_id = std::move(problem_id),
      .language = std::move(language),
      .source_code = std::move(source),
  });
}

algorithm_trainer::Verdict require_verdict(algorithm_trainer::JudgeResult result) {
  if (const auto *error = std::get_if<algorithm_trainer::JudgeError>(&result)) {
    SKIP("NsJail is unavailable in this kernel/environment: " << error->message);
  }
  if (const auto *runtime_error = std::get_if<algorithm_trainer::RuntimeError>(&result)) {
    FAIL("Submission ended with " << runtime_error->type);
  }
  return std::get<algorithm_trainer::Verdict>(result);
}

} // namespace

TEST_CASE("NsJail executes a correct A+B submission", "[sandbox]") {
  const auto verdict = require_verdict(judge("a, b = map(int, input().split())\nprint(a + b)\n"));
  CHECK(verdict == algorithm_trainer::Verdict::accepted);
}

TEST_CASE("NsJail accepts reference solutions for every added problem", "[sandbox][catalog]") {
  const std::vector<std::pair<std::string, std::string>> solutions{
      {"merge-sort", R"(n = int(input())
values = list(map(int, input().split()))
print(*sorted(values))
)"},
      {"activity-selection", R"(n = int(input())
activities = [tuple(map(int, input().split())) for _ in range(n)]
activities.sort(key=lambda interval: (interval[1], interval[0]))
last_end = -10**30
answer = 0
for start, end in activities:
    if start >= last_end:
        answer += 1
        last_end = end
print(answer)
)"},
      {"assign-cookies", R"(n, m = map(int, input().split())
appetites = sorted(map(int, input().split()))
cookies = sorted(map(int, input().split()))
child = 0
for cookie in cookies:
    if child < n and cookie >= appetites[child]:
        child += 1
print(child)
)"},
      {"minimum-arrows", R"(n = int(input())
balloons = [tuple(map(int, input().split())) for _ in range(n)]
balloons.sort(key=lambda interval: (interval[1], interval[0]))
arrows = 0
position = 0
for start, end in balloons:
    if arrows == 0 or start > position:
        arrows += 1
        position = end
print(arrows)
)"},
      {"distinct-values", R"(n = int(input())
print(len(set(map(int, input().split()))))
)"},
      {"first-unique", R"(from collections import Counter
n = int(input())
values = list(map(int, input().split()))
counts = Counter(values)
print(next((value for value in values if counts[value] == 1), -1))
)"},
      {"pair-sum-count", R"(n, target = map(int, input().split())
values = map(int, input().split())
seen = {}
answer = 0
for value in values:
    answer += seen.get(target - value, 0)
    seen[value] = seen.get(value, 0) + 1
print(answer)
)"},
  };

  for (const auto &[problem_id, source] : solutions) {
    CAPTURE(problem_id);
    CHECK(require_verdict(judge(source, "python", problem_id)) ==
          algorithm_trainer::Verdict::accepted);
  }
}

TEST_CASE("Merge Sort rejects a quadratic implementation on its large case", "[sandbox][catalog]") {
  const auto result = judge(R"(n = int(input())
values = list(map(int, input().split()))
for index in range(1, n):
    value = values[index]
    position = index
    while position > 0 and values[position - 1] > value:
        values[position] = values[position - 1]
        position -= 1
    values[position] = value
print(*values)
)",
                            "python", "merge-sort");

  CHECK(require_verdict(result) == algorithm_trainer::Verdict::time_limit_exceeded);
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
