#include "algorithm-trainer/problem-service.h"

namespace algorithm_trainer {
ProblemService::ProblemService(ProblemRepository &repository) : repository_{repository} {}
ProblemListResult ProblemService::list() { return repository_.list_enabled(); }
ProblemFindResult ProblemService::find(const std::string &id) {
  return repository_.find_enabled(id);
}
ProblemFindResult ProblemService::find_for_judging(const std::string &id) {
  return repository_.find_for_judging(id);
}
} // namespace algorithm_trainer
