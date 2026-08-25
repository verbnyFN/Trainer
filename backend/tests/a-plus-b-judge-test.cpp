#include "algorithm-trainer/a-plus-b-judge.h"
#include "algorithm-trainer/executor.h"
#include "algorithm-trainer/judge.h"
#include "scripted-executor.h"

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <string>
#include <utility>
#include <variant>

namespace {

using algorithm_trainer::ExecutionResult;
using algorithm_trainer::ExecutorError;
using algorithm_trainer::ExecutorResult;
using algorithm_trainer::JudgeError;
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
  return {
      completed("5\n"),
      completed("0\n"),
      completed("-12\n"),
      completed("3000000000\n"),
  };
}

Verdict verdict(const algorithm_trainer::JudgeResult &result) {
  REQUIRE(std::holds_alternative<Verdict>(result));
  return std::get<Verdict>(result);
}

} // namespace

TEST_CASE("APlusBJudge accepts when every hidden case is correct", "[a-plus-b-judge]") {
  ScriptedExecutor executor{correct_results()};
  algorithm_trainer::APlusBJudge judge{executor};

  CHECK(verdict(judge.run(submission())) == Verdict::accepted);
  CHECK(executor.call_count() == 4);
  CHECK(executor.requests.front().language == "python");
  CHECK(executor.requests.front().source_code == submission().source_code);
}

TEST_CASE("APlusBJudge rejects an incorrect first result", "[a-plus-b-judge]") {
  auto results = correct_results();
  results.front() = completed("6\n");
  ScriptedExecutor executor{std::move(results)};
  algorithm_trainer::APlusBJudge judge{executor};

  CHECK(verdict(judge.run(submission())) == Verdict::wrong_answer);
  CHECK(executor.call_count() == 1);
}

TEST_CASE("APlusBJudge rejects an incorrect later result", "[a-plus-b-judge]") {
  auto results = correct_results();
  results[2] = completed("12\n");
  ScriptedExecutor executor{std::move(results)};
  algorithm_trainer::APlusBJudge judge{executor};

  CHECK(verdict(judge.run(submission())) == Verdict::wrong_answer);
  CHECK(executor.call_count() == 3);
}

TEST_CASE("APlusBJudge translates a non-zero exit into Runtime Error", "[a-plus-b-judge]") {
  ScriptedExecutor executor{{completed({}, 1, "traceback")}};
  algorithm_trainer::APlusBJudge judge{executor};

  CHECK(verdict(judge.run(submission())) == Verdict::runtime_error);
  CHECK(executor.call_count() == 1);
}

TEST_CASE("APlusBJudge gives timeout precedence over exit status", "[a-plus-b-judge]") {
  auto result = timed_out();
  result.exit_code = 137;
  ScriptedExecutor executor{{result}};
  algorithm_trainer::APlusBJudge judge{executor};

  CHECK(verdict(judge.run(submission())) == Verdict::time_limit_exceeded);
  CHECK(executor.call_count() == 1);
}

TEST_CASE("APlusBJudge propagates executor infrastructure errors", "[a-plus-b-judge]") {
  ScriptedExecutor executor{{ExecutorError{"sandbox unavailable"}}};
  algorithm_trainer::APlusBJudge judge{executor};

  const auto result = judge.run(submission());

  REQUIRE(std::holds_alternative<JudgeError>(result));
  CHECK(std::get<JudgeError>(result).message == "sandbox unavailable");
  CHECK(executor.call_count() == 1);
}

TEST_CASE("APlusBJudge normalizes line endings and trailing whitespace", "[a-plus-b-judge]") {
  auto results = correct_results();
  results[0] = completed("5 \t\r\n\r\n");
  results[1] = completed("0\r");
  ScriptedExecutor executor{std::move(results)};
  algorithm_trainer::APlusBJudge judge{executor};

  CHECK(verdict(judge.run(submission())) == Verdict::accepted);
  CHECK(executor.call_count() == 4);
}

TEST_CASE("APlusBJudge does not use fuzzy output comparison", "[a-plus-b-judge]") {
  auto results = correct_results();
  results.front() = completed("05\n");
  ScriptedExecutor executor{std::move(results)};
  algorithm_trainer::APlusBJudge judge{executor};

  CHECK(verdict(judge.run(submission())) == Verdict::wrong_answer);
  CHECK(executor.call_count() == 1);
}
