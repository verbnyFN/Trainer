#include "algorithm-trainer/judge.h"
#include "algorithm-trainer/mock-judge.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>
#include <utility>
#include <variant>

TEST_CASE("every public verdict has a stable API name", "[judge]") {
  constexpr std::array verdicts{
      std::pair{algorithm_trainer::Verdict::accepted, std::string_view{"Accepted"}},
      std::pair{algorithm_trainer::Verdict::wrong_answer, std::string_view{"Wrong Answer"}},
      std::pair{algorithm_trainer::Verdict::runtime_error, std::string_view{"Runtime Error"}},
      std::pair{algorithm_trainer::Verdict::time_limit_exceeded,
                std::string_view{"Time Limit Exceeded"}},
  };

  for (const auto &[verdict, name] : verdicts) {
    CHECK(algorithm_trainer::verdict_name(verdict) == name);
  }
}

TEST_CASE("MockJudge accepts without executing source code", "[judge]") {
  algorithm_trainer::MockJudge judge;
  const auto result = judge.run({
      .problem_id = "a-plus-b",
      .language = "python",
      .source_code = "this is deliberately not valid Python",
  });

  REQUIRE(std::holds_alternative<algorithm_trainer::Verdict>(result));
  CHECK(std::get<algorithm_trainer::Verdict>(result) == algorithm_trainer::Verdict::accepted);
}
