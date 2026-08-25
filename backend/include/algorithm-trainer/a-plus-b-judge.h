#pragma once

#include "algorithm-trainer/executor.h"
#include "algorithm-trainer/judge.h"

namespace algorithm_trainer {

class APlusBJudge final : public Judge {
public:
  explicit APlusBJudge(Executor &executor);

  JudgeResult run(const JudgeRequest &request) override;

private:
  Executor &executor_;
};

} // namespace algorithm_trainer
