#include "algorithm-trainer/problem.h"

#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace algorithm_trainer {
namespace {

const Problem a_plus_b{
    .id = "a-plus-b",
    .title = "A + B",
    .description = "Read two integers, a and b, and print their sum.",
    .input_format = "One line containing two integers separated by a space.",
    .output_format = "Print one integer: a + b.",
    .difficulty = ProblemDifficulty::easy,
    .tags = {"math", "implementation"},
    .languages = {"python", "cpp"},
    .examples = {{.input = "2 3\n", .output = "5\n"}},
};

} // namespace

std::string_view difficulty_name(ProblemDifficulty difficulty) {
  switch (difficulty) {
  case ProblemDifficulty::easy:
    return "Easy";
  case ProblemDifficulty::medium:
    return "Medium";
  case ProblemDifficulty::hard:
    return "Hard";
  }
  throw std::logic_error{"Unknown problem difficulty"};
}

const std::vector<Problem> &all_problems() {
  static const std::vector problems{a_plus_b};
  return problems;
}

const Problem *find_problem(std::string_view slug) {
  const auto &problems = all_problems();
  const auto found = std::ranges::find(problems, slug, &Problem::id);
  return found == problems.end() ? nullptr : &*found;
}

Json::Value problem_summary_to_json(const Problem &problem) {
  Json::Value json;
  json["id"] = problem.id;
  json["title"] = problem.title;
  json["difficulty"] = std::string{difficulty_name(problem.difficulty)};
  json["tags"] = Json::Value{Json::arrayValue};
  for (const auto &tag : problem.tags) {
    json["tags"].append(tag);
  }
  return json;
}

Json::Value problem_to_json(const Problem &problem) {
  Json::Value json;
  json["id"] = problem.id;
  json["title"] = problem.title;
  json["description"] = problem.description;
  json["inputFormat"] = problem.input_format;
  json["outputFormat"] = problem.output_format;
  json["difficulty"] = std::string{difficulty_name(problem.difficulty)};
  json["tags"] = problem_summary_to_json(problem)["tags"];

  Json::Value languages{Json::arrayValue};
  for (const auto &language : problem.languages) {
    languages.append(language);
  }
  json["languages"] = std::move(languages);

  Json::Value examples{Json::arrayValue};
  for (const auto &example : problem.examples) {
    Json::Value serialized_example;
    serialized_example["input"] = example.input;
    serialized_example["output"] = example.output;
    examples.append(std::move(serialized_example));
  }
  json["examples"] = std::move(examples);
  return json;
}

} // namespace algorithm_trainer
