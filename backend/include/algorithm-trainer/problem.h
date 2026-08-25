#pragma once

#include <json/value.h>

#include <string>
#include <string_view>
#include <vector>

namespace algorithm_trainer {

struct ProblemExample {
  std::string input;
  std::string output;
};

struct Problem {
  std::string id;
  std::string title;
  std::string description;
  std::string input_format;
  std::string output_format;
  std::vector<std::string> languages;
  std::vector<ProblemExample> examples;
};

[[nodiscard]] const Problem *find_problem(std::string_view slug);
[[nodiscard]] Json::Value problem_to_json(const Problem &problem);

} // namespace algorithm_trainer
