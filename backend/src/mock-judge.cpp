#include "algorithm-trainer/mock-judge.h"

namespace algorithm_trainer {

JudgeResult MockJudge::run(const JudgeRequest &request) {
  static_cast<void>(request);
  return Verdict::accepted;
}

} // namespace algorithm_trainer
