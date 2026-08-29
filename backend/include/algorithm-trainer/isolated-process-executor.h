#pragma once

#include "algorithm-trainer/executor.h"

#include <filesystem>
#include <optional>
#include <string>

namespace algorithm_trainer {

class IsolatedProcessExecutor final : public Executor {
public:
  explicit IsolatedProcessExecutor(std::filesystem::path worker_path);

  [[nodiscard]] const std::optional<std::string> &initialization_error() const;
  ExecutorResult run(const ExecutionRequest &request) override;

private:
  std::filesystem::path worker_path_;
  std::filesystem::path cgroupv2_mount_;
  std::optional<std::string> delegation_error_;
};

} // namespace algorithm_trainer
