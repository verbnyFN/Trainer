#pragma once

#include "algorithm-trainer/submission-repository.h"

#include <memory>
#include <string>

namespace algorithm_trainer {

struct PostgreSQLRepositoryLimits {
  std::size_t maximum_active_submissions{100};
  std::size_t maximum_active_submissions_per_user{5};
  std::size_t maximum_running_submissions_per_user{1};
};

class PostgreSQLSubmissionRepository final : public SubmissionRepository {
public:
  using OpenResult = std::variant<std::unique_ptr<PostgreSQLSubmissionRepository>, RepositoryError>;
  ~PostgreSQLSubmissionRepository() override;
  [[nodiscard]] static OpenResult open(const std::string &connection_info,
                                       PostgreSQLRepositoryLimits limits = {});
  StoreSubmissionResult create(const SubmissionRequest &request) override;
  StoreSubmissionResult complete(SubmissionId id, Verdict verdict,
                                 std::optional<std::string> error_type) override;
  StoreSubmissionResult fail(SubmissionId id, std::optional<std::string> error_type) override;
  ClaimSubmissionResult claim_next() override;
  RecoverSubmissionsResult recover_running() override;
  FindSubmissionResult find(SubmissionId id) override;
  SubmissionHistoryResult history(std::int64_t user_id, const std::string &problem_id) override;
  UserProgressResult progress(std::int64_t user_id) override;
  SubmissionHistoryResult list_all() override;
  SubmissionHistoryResult list_filtered(const SubmissionAdminFilter &filter) override;
  StoreSubmissionResult retry(SubmissionId id, std::int64_t admin_user_id) override;
  std::variant<std::monostate, RepositoryError>
  audit(std::int64_t admin_user_id, const std::string &action, const std::string &entity_type,
        const std::string &entity_id, const std::string &details = "{}") override;

private:
  struct Implementation;
  explicit PostgreSQLSubmissionRepository(std::unique_ptr<Implementation> implementation);
  std::unique_ptr<Implementation> implementation_;
};

} // namespace algorithm_trainer
