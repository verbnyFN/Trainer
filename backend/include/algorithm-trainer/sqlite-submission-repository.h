#pragma once

#include "algorithm-trainer/submission-repository.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <variant>

namespace algorithm_trainer {

struct SubmissionQueueLimits {
  std::size_t maximum_active_submissions{100};
  std::size_t maximum_active_submissions_per_user{5};
  std::size_t maximum_running_submissions_per_user{1};
};

class SQLiteSubmissionRepository final : public SubmissionRepository {
public:
  using OpenResult = std::variant<std::unique_ptr<SQLiteSubmissionRepository>, RepositoryError>;

  ~SQLiteSubmissionRepository() override;

  SQLiteSubmissionRepository(const SQLiteSubmissionRepository &) = delete;
  SQLiteSubmissionRepository &operator=(const SQLiteSubmissionRepository &) = delete;

  [[nodiscard]] static OpenResult open(const std::filesystem::path &database_path,
                                       SubmissionQueueLimits limits = {});

  StoreSubmissionResult create(const SubmissionRequest &request) override;
  StoreSubmissionResult complete(SubmissionId submission_id, Verdict verdict,
                                 std::optional<std::string> error_type = std::nullopt) override;
  StoreSubmissionResult fail(SubmissionId submission_id,
                             std::optional<std::string> error_type = std::nullopt) override;
  ClaimSubmissionResult claim_next() override;
  RecoverSubmissionsResult recover_running() override;
  FindSubmissionResult find(SubmissionId submission_id) override;
  SubmissionHistoryResult history(std::int64_t user_id, const std::string &problem_id) override;
  UserProgressResult progress(std::int64_t user_id) override;
  SubmissionHistoryResult list_all() override;
  SubmissionHistoryResult list_filtered(const SubmissionAdminFilter &filter) override;
  StoreSubmissionResult retry(SubmissionId submission_id, std::int64_t admin_user_id) override;
  std::variant<std::monostate, RepositoryError>
  audit(std::int64_t admin_user_id, const std::string &action, const std::string &entity_type,
        const std::string &entity_id, const std::string &details_json = "{}") override;

private:
  struct Implementation;
  explicit SQLiteSubmissionRepository(std::unique_ptr<Implementation> implementation);

  std::unique_ptr<Implementation> implementation_;
};

} // namespace algorithm_trainer
