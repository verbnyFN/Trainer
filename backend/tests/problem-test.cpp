#include "algorithm-trainer/problem.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("the A+B problem can be found by slug", "[problem]") {
  const auto *problem = algorithm_trainer::find_problem("a-plus-b");

  REQUIRE(problem != nullptr);
  CHECK(problem->id == "a-plus-b");
  CHECK(problem->title == "A + B");
  CHECK(problem->difficulty == algorithm_trainer::ProblemDifficulty::easy);
  CHECK(problem->tags == std::vector<std::string>{"math", "implementation"});
  CHECK(problem->languages == std::vector<std::string>{"python", "cpp"});
  REQUIRE(problem->examples.size() == 1);
  CHECK(problem->examples.front().input == "2 3\n");
  CHECK(problem->examples.front().output == "5\n");
}

TEST_CASE("unknown problem slugs are not found", "[problem]") {
  CHECK(algorithm_trainer::find_problem("unknown") == nullptr);
  CHECK(algorithm_trainer::find_problem("") == nullptr);
}

TEST_CASE("the public problem JSON contract contains no hidden tests", "[problem]") {
  const auto *problem = algorithm_trainer::find_problem("a-plus-b");
  REQUIRE(problem != nullptr);

  const auto json = algorithm_trainer::problem_to_json(*problem);

  CHECK(json["id"].asString() == "a-plus-b");
  CHECK(json["title"].asString() == "A + B");
  CHECK(json["description"].isString());
  CHECK(json["inputFormat"].isString());
  CHECK(json["outputFormat"].isString());
  CHECK(json["languages"].isArray());
  CHECK(json["examples"].isArray());
  CHECK_FALSE(json.isMember("testCases"));
  CHECK_FALSE(json.isMember("hiddenTests"));
  CHECK(json["difficulty"].asString() == "Easy");
  CHECK(json["tags"].size() == 2);
  CHECK(json.getMemberNames().size() == 9);
}

TEST_CASE("problem summaries expose catalog metadata", "[problem]") {
  const auto &problems = algorithm_trainer::all_problems();

  REQUIRE(problems.size() == 1);
  const auto summary = algorithm_trainer::problem_summary_to_json(problems.front());
  CHECK(summary["id"].asString() == "a-plus-b");
  CHECK(summary["title"].asString() == "A + B");
  CHECK(summary["difficulty"].asString() == "Easy");
  CHECK(summary["tags"].isArray());
  CHECK(summary["tags"].size() >= 2);
  CHECK(summary["tags"].size() <= 5);
  CHECK(summary.getMemberNames().size() == 4);
}
