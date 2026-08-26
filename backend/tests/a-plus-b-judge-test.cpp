#include "algorithm-trainer/executor.h"
#include "algorithm-trainer/judge.h"
#include "algorithm-trainer/problem-judge.h"
#include "algorithm-trainer/problem.h"
#include "scripted-executor.h"

#include <catch2/catch_test_macros.hpp>

#include <csignal>
#include <deque>
#include <string>
#include <utility>
#include <variant>

namespace {

using algorithm_trainer::ExecutionResult;
using algorithm_trainer::ExecutorError;
using algorithm_trainer::ExecutorResult;
using algorithm_trainer::JudgeError;
using algorithm_trainer::RuntimeError;
using algorithm_trainer::Verdict;

algorithm_trainer::JudgeRequest submission() {
  return {
      .problem_id = "a-plus-b",
      .language = "python",
      .source_code = "submitted source is never executed by this test",
  };
}

ExecutionResult completed(std::string output, int exit_code = 0, std::string error_output = {}) {
  return {
      .exit_code = exit_code,
      .standard_output = std::move(output),
      .standard_error = std::move(error_output),
      .timed_out = false,
  };
}

ExecutionResult timed_out() {
  return {
      .exit_code = 0,
      .standard_output = {},
      .standard_error = {},
      .timed_out = true,
  };
}

std::deque<ExecutorResult> correct_results() {
  std::deque<ExecutorResult> results;
  for (const auto &test : algorithm_trainer::find_problem("a-plus-b")->hidden_tests) {
    results.push_back(completed(test.expected_output));
  }
  return results;
}

Verdict verdict(const algorithm_trainer::JudgeResult &result) {
  REQUIRE(std::holds_alternative<Verdict>(result));
  return std::get<Verdict>(result);
}

} // namespace

TEST_CASE("ProblemJudge accepts when every hidden case is correct", "[problem-judge]") {
  ScriptedExecutor executor{correct_results()};
  algorithm_trainer::ProblemJudge judge{executor};

  CHECK(verdict(judge.run(submission())) == Verdict::accepted);
  CHECK(executor.call_count() == 10);
  CHECK(executor.requests.front().language == "python");
  CHECK(executor.requests.front().source_code == submission().source_code);
}

TEST_CASE("ProblemJudge rejects an incorrect first result", "[problem-judge]") {
  auto results = correct_results();
  results.front() = completed("6\n");
  ScriptedExecutor executor{std::move(results)};
  algorithm_trainer::ProblemJudge judge{executor};

  CHECK(verdict(judge.run(submission())) == Verdict::wrong_answer);
  CHECK(executor.call_count() == 1);
}

TEST_CASE("ProblemJudge rejects an incorrect later result", "[problem-judge]") {
  auto results = correct_results();
  results[2] = completed("12\n");
  ScriptedExecutor executor{std::move(results)};
  algorithm_trainer::ProblemJudge judge{executor};

  CHECK(verdict(judge.run(submission())) == Verdict::wrong_answer);
  CHECK(executor.call_count() == 3);
}

TEST_CASE("ProblemJudge translates a non-zero exit into Runtime Error", "[problem-judge]") {
  ScriptedExecutor executor{{completed({}, 1, "Traceback\nZeroDivisionError: division by zero\n")}};
  algorithm_trainer::ProblemJudge judge{executor};

  const auto result = judge.run(submission());
  REQUIRE(std::holds_alternative<RuntimeError>(result));
  CHECK(std::get<RuntimeError>(result).type == "Exception: ZeroDivisionError");
  CHECK(executor.call_count() == 1);
}

TEST_CASE("ProblemJudge identifies normalized runtime error categories", "[problem-judge]") {
  const auto error_type = [](ExecutionResult execution) {
    ScriptedExecutor executor{{std::move(execution)}};
    algorithm_trainer::ProblemJudge judge{executor};
    const auto result = judge.run(submission());
    REQUIRE(std::holds_alternative<RuntimeError>(result));
    return std::get<RuntimeError>(result).type;
  };

  CHECK(error_type(completed({}, 1, "SyntaxError: invalid syntax\n")) == "Syntax Error");
  CHECK(error_type(completed({}, 1, "MemoryError\n")) == "Memory Limit Exceeded");
  CHECK(error_type(completed({}, 120)) == "Output Limit Exceeded");
  CHECK(error_type(completed({}, 127)) == "Sandbox Runtime Error");
  CHECK(error_type(completed({}, 128 + SIGSEGV)) == "Terminated by SIGSEGV");
  CHECK(error_type(completed({}, 2)) == "Runtime Error");
}

TEST_CASE("ProblemJudge gives timeout precedence over exit status", "[problem-judge]") {
  auto result = timed_out();
  result.exit_code = 137;
  ScriptedExecutor executor{{result}};
  algorithm_trainer::ProblemJudge judge{executor};

  CHECK(verdict(judge.run(submission())) == Verdict::time_limit_exceeded);
  CHECK(executor.call_count() == 1);
}

TEST_CASE("ProblemJudge propagates executor infrastructure errors", "[problem-judge]") {
  ScriptedExecutor executor{{ExecutorError{"sandbox unavailable"}}};
  algorithm_trainer::ProblemJudge judge{executor};

  const auto result = judge.run(submission());

  REQUIRE(std::holds_alternative<JudgeError>(result));
  CHECK(std::get<JudgeError>(result).message == "sandbox unavailable");
  CHECK(executor.call_count() == 1);
}

TEST_CASE("ProblemJudge normalizes line endings and trailing whitespace", "[problem-judge]") {
  auto results = correct_results();
  results[0] = completed("5 \t\r\n\r\n");
  results[1] = completed("0\r");
  ScriptedExecutor executor{std::move(results)};
  algorithm_trainer::ProblemJudge judge{executor};

  CHECK(verdict(judge.run(submission())) == Verdict::accepted);
  CHECK(executor.call_count() == 10);
}

TEST_CASE("ProblemJudge does not use fuzzy output comparison", "[problem-judge]") {
  auto results = correct_results();
  results.front() = completed("05\n");
  ScriptedExecutor executor{std::move(results)};
  algorithm_trainer::ProblemJudge judge{executor};

  CHECK(verdict(judge.run(submission())) == Verdict::wrong_answer);
  CHECK(executor.call_count() == 1);
}
