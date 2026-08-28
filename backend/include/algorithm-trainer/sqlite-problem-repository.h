#pragma once

#include "algorithm-trainer/problem-repository.h"

#include <filesystem>
#include <memory>
#include <variant>

namespace algorithm_trainer {

class SQLiteProblemRepository final : public ProblemRepository {
public:
  using OpenResult = std::variant<std::unique_ptr<SQLiteProblemRepository>, ProblemRepositoryError>;
  ~SQLiteProblemRepository() override;

  [[nodiscard]] static OpenResult open(const std::filesystem::path &database_path);
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
  explicit SQLiteProblemRepository(std::unique_ptr<Implementation> implementation);
  std::unique_ptr<Implementation> implementation_;
};

} // namespace algorithm_trainer
