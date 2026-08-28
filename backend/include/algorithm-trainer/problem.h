#pragma once

#include <json/value.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace algorithm_trainer {

enum class ProblemDifficulty { easy, medium, hard };

[[nodiscard]] std::string_view difficulty_name(ProblemDifficulty difficulty);

struct ProblemExample {
  std::string input;
  std::string output;
};

struct ProblemTestCase {
  std::string input;
  std::string expected_output;
  std::int64_t id{};
  std::int64_t position{};
  bool enabled{true};
  std::int64_t revision{1};
};

struct Problem {
  std::string id;
  std::string title;
  std::string description;
  std::string input_format;
  std::string output_format;
  ProblemDifficulty difficulty{ProblemDifficulty::easy};
  std::vector<std::string> tags;
  std::vector<std::string> languages;
  std::vector<ProblemExample> examples;
  bool enabled{true};
  std::int64_t revision{1};
  std::vector<ProblemTestCase> hidden_tests;
};

[[nodiscard]] Json::Value problem_to_json(const Problem &problem);
[[nodiscard]] Json::Value problem_summary_to_json(const Problem &problem);
[[nodiscard]] Json::Value admin_problem_to_json(const Problem &problem);
[[nodiscard]] Json::Value admin_problem_test_to_json(const ProblemTestCase &test);

} // namespace algorithm_trainer
