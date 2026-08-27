#include "algorithm-trainer/submission-record.h"

#include <stdexcept>

namespace algorithm_trainer {

std::string_view submission_status_name(SubmissionStatus status) {
  switch (status) {
  case SubmissionStatus::queued:
    return "Queued";
  case SubmissionStatus::running:
    return "Running";
  case SubmissionStatus::completed:
    return "Completed";
  case SubmissionStatus::failed:
    return "Failed";
  }
  throw std::logic_error{"Unknown submission status"};
}

} // namespace algorithm_trainer
