#pragma once

#include "algorithm-trainer/problem-repository.h"

namespace algorithm_trainer {

class ProblemService {
public:
  explicit ProblemService(ProblemRepository &repository);
  ProblemListResult list();
  ProblemFindResult find(const std::string &id);
  ProblemFindResult find_for_judging(const std::string &id);

private:
  ProblemRepository &repository_;
};

} // namespace algorithm_trainer
