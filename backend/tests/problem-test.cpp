#include "algorithm-trainer/problem.h"

#include <catch2/catch_test_macros.hpp>

#include <ranges>

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
  for (const auto &problem : algorithm_trainer::all_problems()) {
    const auto json = algorithm_trainer::problem_to_json(problem);
    CHECK_FALSE(json.isMember("testCases"));
    CHECK_FALSE(json.isMember("hiddenTests"));
    CHECK(json.getMemberNames().size() == 9);
  }

  const auto json =
      algorithm_trainer::problem_to_json(*algorithm_trainer::find_problem("a-plus-b"));
  CHECK(json["id"].asString() == "a-plus-b");
  CHECK(json["title"].asString() == "A + B");
  CHECK(json["description"].isString());
  CHECK(json["inputFormat"].isString());
  CHECK(json["outputFormat"].isString());
  CHECK(json["languages"].isArray());
  CHECK(json["examples"].isArray());
  CHECK(json["difficulty"].asString() == "Easy");
  CHECK(json["tags"].size() == 2);
}

TEST_CASE("problem summaries expose catalog metadata", "[problem]") {
  const auto &problems = algorithm_trainer::all_problems();

  REQUIRE(problems.size() == 8);
  for (const auto &problem : problems) {
    CHECK(problem.hidden_tests.size() >= 10);
    CHECK(problem.tags.size() >= 2);
    CHECK(problem.tags.size() <= 5);
    CHECK_FALSE(problem.description.empty());
    CHECK_FALSE(problem.input_format.empty());
    CHECK_FALSE(problem.output_format.empty());
  }

  const auto summary = algorithm_trainer::problem_summary_to_json(problems.front());
  CHECK(summary["id"].asString() == "a-plus-b");
  CHECK(summary["title"].asString() == "A + B");
  CHECK(summary["difficulty"].asString() == "Easy");
  CHECK(summary["tags"].isArray());
  CHECK(summary["tags"].size() >= 2);
  CHECK(summary["tags"].size() <= 5);
  CHECK(summary.getMemberNames().size() == 4);
}

TEST_CASE("catalog contains merge sort, greedy, and hash-table practice", "[problem]") {
  const auto *merge_sort = algorithm_trainer::find_problem("merge-sort");
  const auto *activity_selection = algorithm_trainer::find_problem("activity-selection");
  const auto *assign_cookies = algorithm_trainer::find_problem("assign-cookies");
  const auto *minimum_arrows = algorithm_trainer::find_problem("minimum-arrows");
  const auto *distinct_values = algorithm_trainer::find_problem("distinct-values");
  const auto *first_unique = algorithm_trainer::find_problem("first-unique");
  const auto *pair_sum_count = algorithm_trainer::find_problem("pair-sum-count");

  REQUIRE(merge_sort != nullptr);
  REQUIRE(activity_selection != nullptr);
  REQUIRE(assign_cookies != nullptr);
  REQUIRE(minimum_arrows != nullptr);
  REQUIRE(distinct_values != nullptr);
  REQUIRE(first_unique != nullptr);
  REQUIRE(pair_sum_count != nullptr);
  CHECK(merge_sort->difficulty == algorithm_trainer::ProblemDifficulty::medium);
  CHECK(std::ranges::find(activity_selection->tags, "greedy") != activity_selection->tags.end());
  CHECK(std::ranges::find(assign_cookies->tags, "greedy") != assign_cookies->tags.end());
  CHECK(std::ranges::find(minimum_arrows->tags, "greedy") != minimum_arrows->tags.end());
  CHECK(std::ranges::find(distinct_values->tags, "hash-table") != distinct_values->tags.end());
  CHECK(std::ranges::find(first_unique->tags, "hash-table") != first_unique->tags.end());
  CHECK(std::ranges::find(pair_sum_count->tags, "hash-table") != pair_sum_count->tags.end());
}
