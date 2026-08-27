#pragma once

#include "algorithm-trainer/judge.h"
#include "algorithm-trainer/submission-repository.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace algorithm_trainer {

class SubmissionWorkerPool {
public:
  SubmissionWorkerPool(Judge &judge, SubmissionRepository &repository, std::size_t worker_count,
                       std::chrono::milliseconds idle_poll_interval = std::chrono::milliseconds{
                           250});
  ~SubmissionWorkerPool();

  SubmissionWorkerPool(const SubmissionWorkerPool &) = delete;
  SubmissionWorkerPool &operator=(const SubmissionWorkerPool &) = delete;

  [[nodiscard]] std::optional<std::string> start();
  void stop();
  void notify();

private:
  enum class WorkResult { processed, idle, repository_error };

  WorkResult process_one();
  void worker_loop();

  Judge &judge_;
  SubmissionRepository &repository_;
  std::size_t worker_count_;
  std::chrono::milliseconds idle_poll_interval_;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool stopping_{};
  bool started_{};
  std::vector<std::thread> workers_;
};

} // namespace algorithm_trainer
