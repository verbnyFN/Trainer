#pragma once

#include "algorithm-trainer/problem-repository.h"
#include "algorithm-trainer/problem-seed.h"
#include "algorithm-trainer/problem-service.h"

#include <algorithm>
#include <ranges>

class SeedProblemRepository final : public algorithm_trainer::ProblemRepository {
public:
  algorithm_trainer::ProblemListResult list_enabled() override {
    auto problems = algorithm_trainer::default_problems();
    for (auto &problem : problems)
      problem.hidden_tests.clear();
    return problems;
  }

  algorithm_trainer::ProblemFindResult find_enabled(const std::string &id) override {
    auto found = find(id);
    if (found && !found->enabled)
      return std::optional<algorithm_trainer::Problem>{};
    if (found)
      found->hidden_tests.clear();
    return found;
  }

  algorithm_trainer::ProblemFindResult find_for_judging(const std::string &id) override {
    return find(id);
  }

private:
  static std::optional<algorithm_trainer::Problem> find(const std::string &id) {
    const auto &problems = algorithm_trainer::default_problems();
    const auto found = std::ranges::find(problems, id, &algorithm_trainer::Problem::id);
    return found == problems.end() ? std::nullopt : std::optional{*found};
  }
};

inline algorithm_trainer::ProblemService &test_problem_service() {
  static SeedProblemRepository repository;
  static algorithm_trainer::ProblemService service{repository};
  return service;
}
