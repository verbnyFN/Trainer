#pragma once

#include "algorithm-trainer/judge.h"
#include "algorithm-trainer/submission-record.h"
#include "algorithm-trainer/submission-repository.h"
#include "algorithm-trainer/submission.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace algorithm_trainer {

struct SubmissionServiceError {
  std::string message;
};

using SubmitResult = std::variant<SubmissionRecord, SubmissionServiceError>;
using GetSubmissionResult = std::variant<std::optional<SubmissionRecord>, SubmissionServiceError>;
using GetSubmissionHistoryResult =
    std::variant<std::vector<SubmissionRecord>, SubmissionServiceError>;

class SubmissionService {
public:
  SubmissionService(Judge &judge, SubmissionRepository &repository);

  SubmitResult submit(const SubmissionRequest &submission);
  GetSubmissionResult find(SubmissionId submission_id);
  GetSubmissionHistoryResult history(std::int64_t user_id, const std::string &problem_id);

private:
  Judge &judge_;
  SubmissionRepository &repository_;
};

} // namespace algorithm_trainer
