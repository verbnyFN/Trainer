#pragma once

#include "algorithm-trainer/submission-record.h"
#include "algorithm-trainer/submission-repository.h"
#include "algorithm-trainer/submission.h"

#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace algorithm_trainer {

enum class SubmissionServiceErrorCode { internal, rate_limited, queue_full };

struct SubmissionServiceError {
  std::string message;
  SubmissionServiceErrorCode code{SubmissionServiceErrorCode::internal};
};

using SubmitResult = std::variant<SubmissionRecord, SubmissionServiceError>;
using GetSubmissionResult = std::variant<std::optional<SubmissionRecord>, SubmissionServiceError>;
using GetSubmissionHistoryResult =
    std::variant<std::vector<SubmissionRecord>, SubmissionServiceError>;
using GetUserProgressResult = std::variant<UserProgress, SubmissionServiceError>;

class SubmissionService {
public:
  explicit SubmissionService(SubmissionRepository &repository,
                             std::function<void()> queue_notifier = {});

  SubmitResult submit(const SubmissionRequest &submission);
  GetSubmissionResult find(SubmissionId submission_id);
  GetSubmissionHistoryResult history(std::int64_t user_id, const std::string &problem_id);
  GetUserProgressResult progress(std::int64_t user_id);

private:
  SubmissionRepository &repository_;
  std::function<void()> queue_notifier_;
};

} // namespace algorithm_trainer
