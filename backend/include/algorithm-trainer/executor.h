#pragma once

#include <string>
#include <variant>

namespace algorithm_trainer {

struct ExecutionRequest {
  std::string language;
  std::string source_code;
  std::string standard_input;
};

struct ExecutionResult {
  int exit_code{};
  std::string standard_output;
  std::string standard_error;
  bool timed_out{};
};

struct ExecutorError {
  std::string message;
};

using ExecutorResult = std::variant<ExecutionResult, ExecutorError>;

class Executor {
public:
  virtual ~Executor() = default;

  virtual ExecutorResult run(const ExecutionRequest &request) = 0;
};

} // namespace algorithm_trainer
