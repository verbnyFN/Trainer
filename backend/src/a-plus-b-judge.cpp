#include "algorithm-trainer/a-plus-b-judge.h"

#include <array>
#include <string>
#include <string_view>
#include <variant>

namespace algorithm_trainer {
namespace {

struct TestCase {
  std::string_view input;
  std::string_view expected_output;
};

constexpr std::array test_cases{
    TestCase{.input = "2 3\n", .expected_output = "5\n"},
    TestCase{.input = "0 0\n", .expected_output = "0\n"},
    TestCase{.input = "-7 -5\n", .expected_output = "-12\n"},
    TestCase{.input = "1000000000 2000000000\n", .expected_output = "3000000000\n"},
};

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

APlusBJudge::APlusBJudge(Executor &executor) : executor_{executor} {}

JudgeResult APlusBJudge::run(const JudgeRequest &request) {
  for (const auto &test_case : test_cases) {
    const auto executor_result = executor_.run({
        .language = request.language,
        .source_code = request.source_code,
        .standard_input = std::string{test_case.input},
    });

    if (const auto *error = std::get_if<ExecutorError>(&executor_result)) {
      return JudgeError{error->message};
    }

    const auto &execution = std::get<ExecutionResult>(executor_result);
    if (execution.timed_out) {
      return Verdict::time_limit_exceeded;
    }
    if (execution.exit_code != 0) {
      return Verdict::runtime_error;
    }
    if (normalize_output(execution.standard_output) !=
        normalize_output(test_case.expected_output)) {
      return Verdict::wrong_answer;
    }
  }

  return Verdict::accepted;
}

} // namespace algorithm_trainer
