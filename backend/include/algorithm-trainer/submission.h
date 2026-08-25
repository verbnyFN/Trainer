#pragma once

#include <json/value.h>

#include <string>
#include <variant>

namespace algorithm_trainer {

struct SubmissionRequest {
  std::string problem_id;
  std::string language;
  std::string code;
};

struct ValidationError {
  std::string message;
};

using SubmissionValidation = std::variant<SubmissionRequest, ValidationError>;

SubmissionValidation validate_submission(const Json::Value &json);

} // namespace algorithm_trainer
