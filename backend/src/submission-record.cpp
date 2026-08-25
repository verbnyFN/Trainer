#include "algorithm-trainer/submission-record.h"

#include <stdexcept>

namespace algorithm_trainer {

std::string_view submission_status_name(SubmissionStatus status) {
  switch (status) {
  case SubmissionStatus::pending:
    return "pending";
  case SubmissionStatus::completed:
    return "completed";
  case SubmissionStatus::failed:
    return "failed";
  }
  throw std::logic_error{"Unknown submission status"};
}

} // namespace algorithm_trainer
