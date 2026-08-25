#include "algorithm-trainer/problem.h"

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
    .languages = {"python"},
    .examples = {{.input = "2 3\n", .output = "5\n"}},
};

} // namespace

const Problem *find_problem(std::string_view slug) {
  return slug == a_plus_b.id ? &a_plus_b : nullptr;
}

Json::Value problem_to_json(const Problem &problem) {
  Json::Value json;
  json["id"] = problem.id;
  json["title"] = problem.title;
  json["description"] = problem.description;
  json["inputFormat"] = problem.input_format;
  json["outputFormat"] = problem.output_format;

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
