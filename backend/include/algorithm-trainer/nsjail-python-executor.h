#pragma once

#include "algorithm-trainer/executor.h"

#include <chrono>
#include <cstddef>
#include <filesystem>

namespace algorithm_trainer {

inline constexpr std::chrono::milliseconds default_python_wall_time_limit{2000};
inline constexpr std::size_t default_python_output_limit_bytes{std::size_t{64} * 1024};
inline constexpr int default_python_address_space_megabytes{128};

struct NsJailPythonExecutorConfig {
  std::filesystem::path nsjail_path;
  std::filesystem::path python_path;
  std::filesystem::path runtime_closure_manifest;
  std::chrono::milliseconds wall_time_limit{default_python_wall_time_limit};
  std::size_t output_limit_bytes{default_python_output_limit_bytes};
  int address_space_limit_megabytes{default_python_address_space_megabytes};
  int cpu_time_limit_seconds{3};
  int process_limit{4};
};

class NsJailPythonExecutor final : public Executor {
public:
  NsJailPythonExecutor();
  explicit NsJailPythonExecutor(NsJailPythonExecutorConfig config);

  [[nodiscard]] static NsJailPythonExecutorConfig default_config();

  ExecutorResult run(const ExecutionRequest &request) override;

private:
  NsJailPythonExecutorConfig config_;
};

} // namespace algorithm_trainer
