#pragma once

#include <json/value.h>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace algorithm_trainer {

struct SubmissionRequest {
  std::string problem_id;
  std::string language;
  std::string code;
  std::optional<std::int64_t> user_id;
};

struct ValidationError {
  std::string message;
};

using SubmissionValidation = std::variant<SubmissionRequest, ValidationError>;

SubmissionValidation validate_submission(const Json::Value &json);

} // namespace algorithm_trainer
