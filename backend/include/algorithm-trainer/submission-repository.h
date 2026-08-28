#pragma once

#include "algorithm-trainer/judge.h"
#include "algorithm-trainer/submission-record.h"
#include "algorithm-trainer/submission.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace algorithm_trainer {

enum class RepositoryErrorCode { internal, user_submission_limit, queue_full };

struct RepositoryError {
  std::string message;
  RepositoryErrorCode code{RepositoryErrorCode::internal};
};

struct SubmissionAdminFilter {
  std::optional<SubmissionStatus> status;
  std::optional<std::string> error_type;
  std::optional<std::string> language;
  std::optional<std::string> problem_id;
  std::optional<std::int64_t> user_id;
  std::optional<std::string> created_from;
  std::optional<std::string> created_to;
};

using StoreSubmissionResult = std::variant<SubmissionRecord, RepositoryError>;
using FindSubmissionResult = std::variant<std::optional<SubmissionRecord>, RepositoryError>;
using SubmissionHistoryResult = std::variant<std::vector<SubmissionRecord>, RepositoryError>;
using UserProgressResult = std::variant<UserProgress, RepositoryError>;
using ClaimSubmissionResult = std::variant<std::optional<SubmissionRecord>, RepositoryError>;
using RecoverSubmissionsResult = std::variant<std::size_t, RepositoryError>;

class SubmissionRepository {
public:
  virtual ~SubmissionRepository() = default;

  virtual StoreSubmissionResult create(const SubmissionRequest &request) = 0;
  virtual StoreSubmissionResult complete(SubmissionId submission_id, Verdict verdict,
                                         std::optional<std::string> error_type = std::nullopt) = 0;
  virtual StoreSubmissionResult fail(SubmissionId submission_id,
                                     std::optional<std::string> error_type = std::nullopt) = 0;
  virtual ClaimSubmissionResult claim_next() = 0;
  virtual RecoverSubmissionsResult recover_running() = 0;
  virtual FindSubmissionResult find(SubmissionId submission_id) = 0;
  virtual SubmissionHistoryResult history(std::int64_t user_id, const std::string &problem_id) = 0;
  virtual UserProgressResult progress(std::int64_t user_id) = 0;
  virtual SubmissionHistoryResult list_all() { return RepositoryError{"Unsupported operation"}; }
  virtual SubmissionHistoryResult list_filtered(const SubmissionAdminFilter &) {
    return RepositoryError{"Unsupported operation"};
  }
  virtual StoreSubmissionResult retry(SubmissionId, std::int64_t) {
    return RepositoryError{"Unsupported operation"};
  }
  virtual std::variant<std::monostate, RepositoryError> audit(std::int64_t, const std::string &,
                                                              const std::string &,
                                                              const std::string &,
                                                              const std::string & = "{}") {
    return RepositoryError{"Unsupported operation"};
  }
};

} // namespace algorithm_trainer
