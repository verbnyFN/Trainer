#pragma once

#include "algorithm-trainer/problem-repository.h"

#include <memory>
#include <string>

namespace algorithm_trainer {

class PostgreSQLProblemRepository final : public ProblemRepository {
public:
  using OpenResult =
      std::variant<std::unique_ptr<PostgreSQLProblemRepository>, ProblemRepositoryError>;
  ~PostgreSQLProblemRepository() override;
  [[nodiscard]] static OpenResult open(const std::string &connection_info);
  ProblemListResult list_enabled() override;
  ProblemFindResult find_enabled(const std::string &id) override;
  ProblemFindResult find_for_judging(const std::string &id) override;
  ProblemListResult list_all() override;
  ProblemFindResult find_any(const std::string &id) override;
  ProblemWriteResult create(const Problem &problem) override;
  ProblemWriteResult update(const Problem &problem) override;
  ProblemDeleteResult remove(const std::string &id) override;
  ProblemTestWriteResult create_test(const std::string &problem_id,
                                     const ProblemTestCase &test) override;
  ProblemTestWriteResult update_test(const std::string &problem_id,
                                     const ProblemTestCase &test) override;
  ProblemDeleteResult remove_test(const std::string &problem_id, std::int64_t test_id) override;

private:
  struct Implementation;
  explicit PostgreSQLProblemRepository(std::unique_ptr<Implementation> implementation);
  std::unique_ptr<Implementation> implementation_;
};

} // namespace algorithm_trainer
