#pragma once

#include "algorithm-trainer/submission-repository.h"

#include <filesystem>
#include <memory>
#include <variant>

namespace algorithm_trainer {

class SQLiteSubmissionRepository final : public SubmissionRepository {
public:
  using OpenResult = std::variant<std::unique_ptr<SQLiteSubmissionRepository>, RepositoryError>;

  ~SQLiteSubmissionRepository() override;

  SQLiteSubmissionRepository(const SQLiteSubmissionRepository &) = delete;
  SQLiteSubmissionRepository &operator=(const SQLiteSubmissionRepository &) = delete;

  [[nodiscard]] static OpenResult open(const std::filesystem::path &database_path);

  StoreSubmissionResult create(const SubmissionRequest &request) override;
  StoreSubmissionResult complete(SubmissionId submission_id, Verdict verdict,
                                 std::optional<std::string> error_type = std::nullopt) override;
  StoreSubmissionResult fail(SubmissionId submission_id,
                             std::optional<std::string> error_type = std::nullopt) override;
  FindSubmissionResult find(SubmissionId submission_id) override;
  SubmissionHistoryResult history(std::int64_t user_id, const std::string &problem_id) override;
  UserProgressResult progress(std::int64_t user_id) override;

private:
  struct Implementation;
  explicit SQLiteSubmissionRepository(std::unique_ptr<Implementation> implementation);

  std::unique_ptr<Implementation> implementation_;
};

} // namespace algorithm_trainer
