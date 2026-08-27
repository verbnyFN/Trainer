#include "algorithm-trainer/submission.h"

#include <cstddef>
#include <string_view>
#include <utility>

namespace algorithm_trainer {
namespace {

constexpr std::string_view problem_id{"a-plus-b"};
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

  if (request.problem_id != problem_id) {
    return error("Unsupported problemId");
  }
  if (request.language != "python" && request.language != "cpp") {
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
