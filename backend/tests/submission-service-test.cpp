#include "algorithm-trainer/submission-repository.h"
#include "algorithm-trainer/submission-service.h"
#include "algorithm-trainer/submission.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

class RecordingRepository final : public algorithm_trainer::SubmissionRepository {
public:
  std::optional<algorithm_trainer::RepositoryError> create_error;
  std::size_t create_count{};
  algorithm_trainer::SubmissionRecord record{
      .id = {42},
      .problem_id = "a-plus-b",
      .language = "python",
      .source_code = "print(3)",
      .status = algorithm_trainer::SubmissionStatus::queued,
      .created_at = "2026-01-01T00:00:00.000Z",
  };

  algorithm_trainer::StoreSubmissionResult
  create(const algorithm_trainer::SubmissionRequest &request) override {
    ++create_count;
    if (create_error) {
      return *create_error;
    }
    record.problem_id = request.problem_id;
    record.language = request.language;
    record.source_code = request.code;
    record.user_id = request.user_id;
    return record;
  }

  algorithm_trainer::StoreSubmissionResult complete(algorithm_trainer::SubmissionId,
                                                    algorithm_trainer::Verdict,
                                                    std::optional<std::string>) override {
    return algorithm_trainer::RepositoryError{"not used"};
  }

  algorithm_trainer::StoreSubmissionResult fail(algorithm_trainer::SubmissionId,
                                                std::optional<std::string>) override {
    return algorithm_trainer::RepositoryError{"not used"};
  }

  algorithm_trainer::ClaimSubmissionResult claim_next() override {
    return std::optional<algorithm_trainer::SubmissionRecord>{};
  }

  algorithm_trainer::RecoverSubmissionsResult recover_running() override { return std::size_t{}; }

  algorithm_trainer::FindSubmissionResult find(algorithm_trainer::SubmissionId id) override {
    return id == record.id ? std::optional{record}
                           : std::optional<algorithm_trainer::SubmissionRecord>{};
  }

  algorithm_trainer::SubmissionHistoryResult history(std::int64_t user_id,
                                                     const std::string &problem_id) override {
    if (record.user_id == user_id && record.problem_id == problem_id) {
      return std::vector{record};
    }
    return std::vector<algorithm_trainer::SubmissionRecord>{};
  }

  algorithm_trainer::UserProgressResult progress(std::int64_t user_id) override {
    if (record.user_id != user_id) {
      return algorithm_trainer::UserProgress{};
    }
    return algorithm_trainer::UserProgress{
        .total_submissions = 1,
        .most_recent_submission_problem_id = record.problem_id,
    };
  }
};

algorithm_trainer::SubmissionRequest valid_submission() {
  return {
      .problem_id = "a-plus-b",
      .language = "python",
      .code = "print(sum(map(int, input().split())))",
      .user_id = 7,
  };
}

} // namespace

TEST_CASE("SubmissionService enqueues without judging synchronously", "[submission-service]") {
  RecordingRepository repository;
  std::size_t notifications{};
  algorithm_trainer::SubmissionService service{repository, [&notifications] { ++notifications; }};

  const auto result = service.submit(valid_submission());

  REQUIRE(std::holds_alternative<algorithm_trainer::SubmissionRecord>(result));
  const auto &queued = std::get<algorithm_trainer::SubmissionRecord>(result);
  CHECK(queued.status == algorithm_trainer::SubmissionStatus::queued);
  CHECK(queued.id.value == 42);
  CHECK(repository.create_count == 1);
  CHECK(notifications == 1);
}

TEST_CASE("SubmissionService reports queue persistence errors", "[submission-service]") {
  RecordingRepository repository;
  repository.create_error = algorithm_trainer::RepositoryError{"database unavailable"};
  algorithm_trainer::SubmissionService service{repository};

  const auto result = service.submit(valid_submission());

  CHECK(std::holds_alternative<algorithm_trainer::SubmissionServiceError>(result));
  CHECK(repository.create_count == 1);
}

TEST_CASE("SubmissionService preserves rate-limit and queue-full errors", "[submission-service]") {
  RecordingRepository repository;
  algorithm_trainer::SubmissionService service{repository};

  repository.create_error = algorithm_trainer::RepositoryError{
      "Too many active submissions for this user",
      algorithm_trainer::RepositoryErrorCode::user_submission_limit};
  const auto limited = service.submit(valid_submission());
  REQUIRE(std::holds_alternative<algorithm_trainer::SubmissionServiceError>(limited));
  CHECK(std::get<algorithm_trainer::SubmissionServiceError>(limited).code ==
        algorithm_trainer::SubmissionServiceErrorCode::rate_limited);

  repository.create_error = algorithm_trainer::RepositoryError{
      "Submission queue is full", algorithm_trainer::RepositoryErrorCode::queue_full};
  const auto full = service.submit(valid_submission());
  REQUIRE(std::holds_alternative<algorithm_trainer::SubmissionServiceError>(full));
  CHECK(std::get<algorithm_trainer::SubmissionServiceError>(full).code ==
        algorithm_trainer::SubmissionServiceErrorCode::queue_full);
}

TEST_CASE("SubmissionService retrieves queued records and history", "[submission-service]") {
  RecordingRepository repository;
  repository.record.user_id = 7;
  algorithm_trainer::SubmissionService service{repository};

  const auto found = service.find({42});
  const auto missing = service.find({99});
  const auto history = service.history(7, "a-plus-b");

  REQUIRE(std::holds_alternative<std::optional<algorithm_trainer::SubmissionRecord>>(found));
  CHECK(std::get<std::optional<algorithm_trainer::SubmissionRecord>>(found)->status ==
        algorithm_trainer::SubmissionStatus::queued);
  CHECK_FALSE(std::get<std::optional<algorithm_trainer::SubmissionRecord>>(missing).has_value());
  REQUIRE(std::holds_alternative<std::vector<algorithm_trainer::SubmissionRecord>>(history));
  CHECK(std::get<std::vector<algorithm_trainer::SubmissionRecord>>(history).size() == 1);
}

TEST_CASE("SubmissionService retrieves user progress", "[submission-service]") {
  RecordingRepository repository;
  repository.record.user_id = 7;
  algorithm_trainer::SubmissionService service{repository};

  const auto result = service.progress(7);

  REQUIRE(std::holds_alternative<algorithm_trainer::UserProgress>(result));
  const auto &progress = std::get<algorithm_trainer::UserProgress>(result);
  CHECK(progress.total_submissions == 1);
  CHECK(progress.accepted_submissions == 0);
  CHECK(progress.completed_problems.empty());
  CHECK(progress.most_recent_submission_problem_id == "a-plus-b");
}
