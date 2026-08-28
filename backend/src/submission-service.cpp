#include "algorithm-trainer/submission-service.h"

#include <optional>
#include <utility>
#include <variant>

namespace algorithm_trainer {

SubmissionService::SubmissionService(SubmissionRepository &repository,
                                     std::function<void()> queue_notifier)
    : repository_{repository}, queue_notifier_{std::move(queue_notifier)} {}

SubmitResult SubmissionService::submit(const SubmissionRequest &submission) {
  auto created = repository_.create(submission);
  if (const auto *error = std::get_if<RepositoryError>(&created)) {
    const auto code = error->code == RepositoryErrorCode::user_submission_limit
                          ? SubmissionServiceErrorCode::rate_limited
                      : error->code == RepositoryErrorCode::queue_full
                          ? SubmissionServiceErrorCode::queue_full
                          : SubmissionServiceErrorCode::internal;
    return SubmissionServiceError{error->message, code};
  }
  auto queued = std::get<SubmissionRecord>(std::move(created));
  if (queue_notifier_) {
    queue_notifier_();
  }
  return queued;
}

GetSubmissionResult SubmissionService::find(SubmissionId submission_id) {
  auto result = repository_.find(submission_id);
  if (const auto *error = std::get_if<RepositoryError>(&result)) {
    return SubmissionServiceError{error->message};
  }
  return std::get<std::optional<SubmissionRecord>>(std::move(result));
}

GetSubmissionHistoryResult SubmissionService::history(std::int64_t user_id,
                                                      const std::string &problem_id) {
  auto result = repository_.history(user_id, problem_id);
  if (const auto *error = std::get_if<RepositoryError>(&result)) {
    return SubmissionServiceError{error->message};
  }
  return std::get<std::vector<SubmissionRecord>>(std::move(result));
}

GetUserProgressResult SubmissionService::progress(std::int64_t user_id) {
  auto result = repository_.progress(user_id);
  if (const auto *error = std::get_if<RepositoryError>(&result)) {
    return SubmissionServiceError{error->message};
  }
  return std::get<UserProgress>(std::move(result));
}

} // namespace algorithm_trainer
