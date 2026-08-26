#include "algorithm-trainer/submission.h"

#include "algorithm-trainer/problem.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace algorithm_trainer {
namespace {

constexpr std::size_t maximum_code_size{64 * 1024};

ValidationError error(std::string message) { return ValidationError{std::move(message)}; }

} // namespace

SubmissionValidation validate_submission(const Json::Value &json) {
  if (!json.isObject()) {
    return error("Request body must be a JSON object");
  }

  for (const auto *field : {"problemId", "language", "code"}) {
    if (!json.isMember(field) || !json[field].isString()) {
      return error(std::string{field} + " must be a string");
    }
  }

  SubmissionRequest request{
      .problem_id = json["problemId"].asString(),
      .language = json["language"].asString(),
      .code = json["code"].asString(),
  };

  const auto *problem = find_problem(request.problem_id);
  if (problem == nullptr) {
    return error("Unsupported problemId");
  }
  if (std::ranges::find(problem->languages, request.language) == problem->languages.end()) {
    return error("Unsupported language");
  }
  if (request.code.empty()) {
    return error("code must not be empty");
  }
  if (request.code.size() > maximum_code_size) {
    return error("code is too large");
  }

  return request;
}

} // namespace algorithm_trainer
