#pragma once

#include "algorithm-trainer/executor.h"
#include "algorithm-trainer/judge.h"

namespace algorithm_trainer {

class ProblemJudge final : public Judge {
public:
  explicit ProblemJudge(Executor &executor);

  JudgeResult run(const JudgeRequest &request) override;

private:
  Executor &executor_;
};

} // namespace algorithm_trainer
