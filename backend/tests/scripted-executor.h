#pragma once

#include "algorithm-trainer/executor.h"

#include <cstddef>
#include <deque>
#include <utility>
#include <vector>

class ScriptedExecutor final : public algorithm_trainer::Executor {
public:
  explicit ScriptedExecutor(std::deque<algorithm_trainer::ExecutorResult> results)
      : results_{std::move(results)} {}

  algorithm_trainer::ExecutorResult
  run(const algorithm_trainer::ExecutionRequest &request) override {
    requests.push_back(request);
    if (results_.empty()) {
      return algorithm_trainer::ExecutorError{"Scripted executor has no result"};
    }

    auto result = std::move(results_.front());
    results_.pop_front();
    return result;
  }

  [[nodiscard]] std::size_t call_count() const { return requests.size(); }

  std::vector<algorithm_trainer::ExecutionRequest> requests;

private:
  std::deque<algorithm_trainer::ExecutorResult> results_;
};
