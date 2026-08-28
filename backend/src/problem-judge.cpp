#include "algorithm-trainer/problem-judge.h"

#include "algorithm-trainer/problem.h"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace algorithm_trainer {
namespace {

std::optional<std::string> python_exception_name(std::string_view error) {
  std::size_t start{};
  while (start < error.size()) {
    const auto end = error.find('\n', start);
    auto line =
        error.substr(start, end == std::string_view::npos ? error.size() - start : end - start);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())) != 0) {
      line.remove_prefix(1);
    }
    const auto separator = line.find(':');
    const auto name = line.substr(0, separator);
    const bool identifier = !name.empty() && std::ranges::all_of(name, [](unsigned char character) {
      return std::isalnum(character) != 0 || character == '_';
    });
    if (identifier && (name.ends_with("Error") || name.ends_with("Exception"))) {
      return std::string{name};
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return std::nullopt;
}

std::string signal_name(int signal) {
  switch (signal) {
  case SIGSEGV:
    return "SIGSEGV";
  case SIGABRT:
    return "SIGABRT";
  case SIGILL:
    return "SIGILL";
  case SIGFPE:
    return "SIGFPE";
  case SIGKILL:
    return "SIGKILL";
  case SIGTERM:
    return "SIGTERM";
  default:
    return "signal " + std::to_string(signal);
  }
}

RuntimeError classify_runtime_error(const ExecutionResult &execution) {
  constexpr int output_limit_exit_code{120};
  constexpr int child_launch_error_exit_code{127};
  constexpr int signal_exit_code_offset{128};
  if (execution.error_type) {
    return {*execution.error_type};
  }
  if (execution.exit_code == output_limit_exit_code) {
    return {"Output Limit Exceeded"};
  }
  if (execution.exit_code == child_launch_error_exit_code) {
    return {"Sandbox Runtime Error"};
  }
  const auto exception = python_exception_name(execution.standard_error);
  if (exception == "MemoryError" ||
      execution.standard_error.find("std::bad_alloc") != std::string::npos) {
    return {"Memory Limit Exceeded"};
  }
  if (exception == "SyntaxError" || exception == "IndentationError" || exception == "TabError") {
    return {"Syntax Error"};
  }
  if (exception) {
    return {"Exception: " + *exception};
  }
  if (execution.exit_code >= signal_exit_code_offset) {
    return {"Terminated by " + signal_name(execution.exit_code - signal_exit_code_offset)};
  }
  return {"Runtime Error"};
}

std::string normalize_output(std::string_view output) {
  std::string normalized;
  normalized.reserve(output.size());
  for (std::size_t index = 0; index < output.size(); ++index) {
    if (output[index] != '\r') {
      normalized.push_back(output[index]);
      continue;
    }
    normalized.push_back('\n');
    if (index + 1 < output.size() && output[index + 1] == '\n') {
      ++index;
    }
  }

  constexpr std::string_view trailing_whitespace{" \t\n\v\f"};
  while (!normalized.empty() &&
         trailing_whitespace.find(normalized.back()) != std::string_view::npos) {
    normalized.pop_back();
  }
  return normalized;
}

} // namespace

ProblemJudge::ProblemJudge(Executor &executor, ProblemService &problems)
    : executor_{executor}, problems_{problems} {}

JudgeResult ProblemJudge::run(const JudgeRequest &request) {
  auto found = problems_.find_for_judging(request.problem_id);
  if (const auto *error = std::get_if<ProblemRepositoryError>(&found)) {
    return JudgeError{error->message};
  }
  auto problem = std::get<std::optional<Problem>>(std::move(found));
  if (!problem) {
    return JudgeError{"Unknown problem"};
  }
  if (problem->hidden_tests.empty()) {
    return JudgeError{"Problem has no enabled hidden tests"};
  }

  for (const auto &test_case : problem->hidden_tests) {
    const auto executor_result = executor_.run({
        .language = request.language,
        .source_code = request.source_code,
        .standard_input = test_case.input,
    });
    if (const auto *error = std::get_if<ExecutorError>(&executor_result)) {
      return JudgeError{error->message};
    }

    const auto &execution = std::get<ExecutionResult>(executor_result);
    if (execution.timed_out) {
      return Verdict::time_limit_exceeded;
    }
    if (execution.exit_code != 0) {
      return classify_runtime_error(execution);
    }
    if (normalize_output(execution.standard_output) !=
        normalize_output(test_case.expected_output)) {
      return Verdict::wrong_answer;
    }
  }
  return Verdict::accepted;
}

} // namespace algorithm_trainer
