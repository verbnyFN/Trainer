#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace algorithm_trainer {

enum class Verdict : std::uint8_t {
  accepted,
  wrong_answer,
  runtime_error,
  time_limit_exceeded,
};

std::string_view verdict_name(Verdict verdict);

struct JudgeRequest {
  std::string problem_id;
  std::string language;
  std::string source_code;
};

struct JudgeError {
  std::string message;
};

using JudgeResult = std::variant<Verdict, JudgeError>;

class Judge {
public:
  virtual ~Judge() = default;

  virtual JudgeResult run(const JudgeRequest &request) = 0;
};

} // namespace algorithm_trainer
