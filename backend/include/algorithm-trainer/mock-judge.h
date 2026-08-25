#pragma once

#include "algorithm-trainer/judge.h"

namespace algorithm_trainer {

class MockJudge final : public Judge {
public:
  JudgeResult run(const JudgeRequest &request) override;
};

} // namespace algorithm_trainer
