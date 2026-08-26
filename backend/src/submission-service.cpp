#include "algorithm-trainer/submission-service.h"

#include <optional>
#include <utility>
#include <variant>

namespace algorithm_trainer {

SubmissionService::SubmissionService(Judge &judge, SubmissionRepository &repository)
    : judge_{judge}, repository_{repository} {}

SubmitResult SubmissionService::submit(const SubmissionRequest &submission) {
  auto created = repository_.create(submission);
  if (const auto *error = std::get_if<RepositoryError>(&created)) {
    return SubmissionServiceError{error->message};
  }
  const auto pending = std::get<SubmissionRecord>(std::move(created));

  const auto judged = judge_.run(JudgeRequest{
      .problem_id = submission.problem_id,
      .language = submission.language,
      .source_code = submission.code,
  });
  if (const auto *error = std::get_if<JudgeError>(&judged)) {
    const auto failed = repository_.fail(pending.id, "Sandbox Error");
    if (const auto *repository_error = std::get_if<RepositoryError>(&failed)) {
      return SubmissionServiceError{
          "Judge failed: " + error->message +
          "; submission could not be marked failed: " + repository_error->message};
    }
    return SubmissionServiceError{"Judge failed: " + error->message};
  }

  const auto *runtime_error = std::get_if<RuntimeError>(&judged);
  const auto verdict =
      runtime_error == nullptr ? std::get<Verdict>(judged) : Verdict::runtime_error;
  auto completed = repository_.complete(
      pending.id, verdict,
      runtime_error == nullptr ? std::nullopt : std::optional<std::string>{runtime_error->type});
  if (const auto *error = std::get_if<RepositoryError>(&completed)) {
    return SubmissionServiceError{error->message};
  }
  return std::get<SubmissionRecord>(std::move(completed));
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
