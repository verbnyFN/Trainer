#pragma once

#include "algorithm-trainer/judge.h"
#include "algorithm-trainer/submission-record.h"
#include "algorithm-trainer/submission.h"

#include <optional>
#include <string>
#include <variant>

namespace algorithm_trainer {

struct RepositoryError {
  std::string message;
};

using StoreSubmissionResult = std::variant<SubmissionRecord, RepositoryError>;
using FindSubmissionResult = std::variant<std::optional<SubmissionRecord>, RepositoryError>;

class SubmissionRepository {
public:
  virtual ~SubmissionRepository() = default;

  virtual StoreSubmissionResult create(const SubmissionRequest &request) = 0;
  virtual StoreSubmissionResult complete(SubmissionId submission_id, Verdict verdict) = 0;
  virtual StoreSubmissionResult fail(SubmissionId submission_id) = 0;
  virtual FindSubmissionResult find(SubmissionId submission_id) = 0;
};

} // namespace algorithm_trainer
