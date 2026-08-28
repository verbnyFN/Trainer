#pragma once

#include "algorithm-trainer/executor.h"
#include "algorithm-trainer/judge.h"
#include "algorithm-trainer/problem-service.h"

namespace algorithm_trainer {

class ProblemJudge final : public Judge {
public:
  ProblemJudge(Executor &executor, ProblemService &problems);

  JudgeResult run(const JudgeRequest &request) override;

private:
  Executor &executor_;
  ProblemService &problems_;
};

} // namespace algorithm_trainer
