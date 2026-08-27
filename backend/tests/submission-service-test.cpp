#include "algorithm-trainer/judge.h"
#include "algorithm-trainer/submission-record.h"
#include "algorithm-trainer/submission-repository.h"
#include "algorithm-trainer/submission-service.h"
#include "algorithm-trainer/submission.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

class RecordingJudge final : public algorithm_trainer::Judge {
public:
  algorithm_trainer::JudgeResult result{algorithm_trainer::Verdict::accepted};
  std::optional<algorithm_trainer::JudgeRequest> received_request;
  std::size_t call_count{};

  algorithm_trainer::JudgeResult run(const algorithm_trainer::JudgeRequest &request) override {
    ++call_count;
    received_request = request;
    return result;
  }
};

class RecordingRepository final : public algorithm_trainer::SubmissionRepository {
public:
  std::optional<algorithm_trainer::RepositoryError> create_error;
  std::size_t create_count{};
  std::size_t complete_count{};
  std::size_t fail_count{};
  std::optional<algorithm_trainer::Verdict> completed_verdict;
  std::optional<std::string> stored_error_type;
  algorithm_trainer::SubmissionRecord record{
      .id = {42},
      .problem_id = "a-plus-b",
      .language = "python",
      .source_code = "print(3)",
      .status = algorithm_trainer::SubmissionStatus::pending,
      .verdict = std::nullopt,
      .created_at = "2026-01-01T00:00:00.000Z",
      .completed_at = std::nullopt,
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
    return record;
  }

  algorithm_trainer::StoreSubmissionResult
  complete(algorithm_trainer::SubmissionId, algorithm_trainer::Verdict verdict,
           std::optional<std::string> error_type) override {
    ++complete_count;
    completed_verdict = verdict;
    record.status = algorithm_trainer::SubmissionStatus::completed;
    record.verdict = verdict;
    record.error_type = error_type;
    stored_error_type = std::move(error_type);
    record.completed_at = "2026-01-01T00:00:01.000Z";
    return record;
  }

  algorithm_trainer::StoreSubmissionResult fail(algorithm_trainer::SubmissionId,
                                                std::optional<std::string> error_type) override {
    ++fail_count;
    record.status = algorithm_trainer::SubmissionStatus::failed;
    record.error_type = error_type;
    stored_error_type = std::move(error_type);
    record.completed_at = "2026-01-01T00:00:01.000Z";
    return record;
  }

  algorithm_trainer::FindSubmissionResult find(algorithm_trainer::SubmissionId id) override {
    if (id == record.id) {
      return std::optional{record};
    }
    return std::optional<algorithm_trainer::SubmissionRecord>{};
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
        .accepted_submissions = record.verdict == algorithm_trainer::Verdict::accepted ? 1 : 0,
        .completed_problems = record.verdict == algorithm_trainer::Verdict::accepted
                                  ? std::vector<algorithm_trainer::CompletedProblem>{{
                                        .problem_id = record.problem_id,
                                        .completed_at = *record.completed_at,
                                    }}
                                  : std::vector<algorithm_trainer::CompletedProblem>{},
    };
  }
};

algorithm_trainer::SubmissionRequest valid_submission() {
  return {
      .problem_id = "a-plus-b",
      .language = "python",
      .code = "print(sum(map(int, input().split())))",
  };
}

Json::Value valid_json() {
  Json::Value json;
  json["problemId"] = "a-plus-b";
  json["language"] = "python";
  json["code"] = "print(3)";
  return json;
}

} // namespace

TEST_CASE("SubmissionService persists before forwarding to its judge", "[submission-service]") {
  RecordingJudge judge;
  RecordingRepository repository;
  algorithm_trainer::SubmissionService service{judge, repository};

  const auto result = service.submit(valid_submission());

  REQUIRE(judge.received_request.has_value());
  CHECK(judge.received_request->problem_id == "a-plus-b");
  CHECK(judge.received_request->language == "python");
  CHECK(judge.received_request->source_code == "print(sum(map(int, input().split())))");
  CHECK(repository.create_count == 1);
  CHECK(repository.complete_count == 1);
  REQUIRE(std::holds_alternative<algorithm_trainer::SubmissionRecord>(result));
  CHECK(std::get<algorithm_trainer::SubmissionRecord>(result).id.value == 42);
}

TEST_CASE("SubmissionService persists every public verdict", "[submission-service]") {
  constexpr std::array verdicts{
      algorithm_trainer::Verdict::accepted,
      algorithm_trainer::Verdict::wrong_answer,
      algorithm_trainer::Verdict::runtime_error,
      algorithm_trainer::Verdict::time_limit_exceeded,
  };

  for (const auto verdict : verdicts) {
    RecordingJudge judge;
    RecordingRepository repository;
    algorithm_trainer::SubmissionService service{judge, repository};
    judge.result = verdict;

    const auto result = service.submit(valid_submission());

    REQUIRE(std::holds_alternative<algorithm_trainer::SubmissionRecord>(result));
    const auto &record = std::get<algorithm_trainer::SubmissionRecord>(result);
    CHECK(record.status == algorithm_trainer::SubmissionStatus::completed);
    CHECK(record.verdict == verdict);
    CHECK(repository.completed_verdict == verdict);
  }
}

TEST_CASE("SubmissionService marks judge infrastructure failures", "[submission-service]") {
  RecordingJudge judge;
  RecordingRepository repository;
  judge.result = algorithm_trainer::JudgeError{"sandbox unavailable"};
  algorithm_trainer::SubmissionService service{judge, repository};

  const auto result = service.submit(valid_submission());

  REQUIRE(std::holds_alternative<algorithm_trainer::SubmissionServiceError>(result));
  CHECK(std::get<algorithm_trainer::SubmissionServiceError>(result).message.find(
            "sandbox unavailable") != std::string::npos);
  CHECK(repository.fail_count == 1);
  CHECK(repository.record.status == algorithm_trainer::SubmissionStatus::failed);
  CHECK(repository.stored_error_type == "Sandbox Error");
}

TEST_CASE("SubmissionService persists a normalized runtime error type", "[submission-service]") {
  RecordingJudge judge;
  RecordingRepository repository;
  judge.result = algorithm_trainer::RuntimeError{"Syntax Error"};
  algorithm_trainer::SubmissionService service{judge, repository};

  const auto result = service.submit(valid_submission());

  REQUIRE(std::holds_alternative<algorithm_trainer::SubmissionRecord>(result));
  const auto &record = std::get<algorithm_trainer::SubmissionRecord>(result);
  CHECK(record.verdict == algorithm_trainer::Verdict::runtime_error);
  CHECK(record.error_type == "Syntax Error");
  CHECK(repository.stored_error_type == "Syntax Error");
}

TEST_CASE("repository creation failures prevent judging", "[submission-service]") {
  RecordingJudge judge;
  RecordingRepository repository;
  repository.create_error = algorithm_trainer::RepositoryError{"database unavailable"};
  algorithm_trainer::SubmissionService service{judge, repository};

  const auto result = service.submit(valid_submission());

  CHECK(std::holds_alternative<algorithm_trainer::SubmissionServiceError>(result));
  CHECK(judge.call_count == 0);
}

TEST_CASE("SubmissionService retrieves persisted records", "[submission-service]") {
  RecordingJudge judge;
  RecordingRepository repository;
  algorithm_trainer::SubmissionService service{judge, repository};

  const auto found = service.find({42});
  const auto missing = service.find({99});

  REQUIRE(std::holds_alternative<std::optional<algorithm_trainer::SubmissionRecord>>(found));
  CHECK(std::get<std::optional<algorithm_trainer::SubmissionRecord>>(found).has_value());
  REQUIRE(std::holds_alternative<std::optional<algorithm_trainer::SubmissionRecord>>(missing));
  CHECK_FALSE(std::get<std::optional<algorithm_trainer::SubmissionRecord>>(missing).has_value());
}

TEST_CASE("SubmissionService retrieves history for one user and problem", "[submission-service]") {
  RecordingJudge judge;
  RecordingRepository repository;
  repository.record.user_id = 7;
  algorithm_trainer::SubmissionService service{judge, repository};

  const auto matching = service.history(7, "a-plus-b");
  const auto other_user = service.history(8, "a-plus-b");
  const auto other_problem = service.history(7, "another-problem");

  REQUIRE(std::holds_alternative<std::vector<algorithm_trainer::SubmissionRecord>>(matching));
  CHECK(std::get<std::vector<algorithm_trainer::SubmissionRecord>>(matching).size() == 1);
  CHECK(std::get<std::vector<algorithm_trainer::SubmissionRecord>>(other_user).empty());
  CHECK(std::get<std::vector<algorithm_trainer::SubmissionRecord>>(other_problem).empty());
}

TEST_CASE("SubmissionService retrieves user progress", "[submission-service]") {
  RecordingJudge judge;
  RecordingRepository repository;
  repository.record.user_id = 7;
  repository.record.verdict = algorithm_trainer::Verdict::accepted;
  repository.record.completed_at = "2026-01-01T00:00:01.000Z";
  algorithm_trainer::SubmissionService service{judge, repository};

  const auto result = service.progress(7);

  REQUIRE(std::holds_alternative<algorithm_trainer::UserProgress>(result));
  const auto &progress = std::get<algorithm_trainer::UserProgress>(result);
  CHECK(progress.total_submissions == 1);
  CHECK(progress.accepted_submissions == 1);
  REQUIRE(progress.completed_problems.size() == 1);
  CHECK(progress.completed_problems.front().problem_id == "a-plus-b");
}

TEST_CASE("invalid submissions never reach persistence or judging", "[submission-service]") {
  RecordingJudge judge;
  RecordingRepository repository;
  algorithm_trainer::SubmissionService service{judge, repository};
  auto json = valid_json();

  SECTION("missing code") { json.removeMember("code"); }
  SECTION("unsupported problem") { json["problemId"] = "another-problem"; }
  SECTION("unsupported language") { json["language"] = "rust"; }
  SECTION("empty code") { json["code"] = ""; }

  const auto validation = algorithm_trainer::validate_submission(json);
  if (const auto *submission = std::get_if<algorithm_trainer::SubmissionRequest>(&validation)) {
    service.submit(*submission);
  }

  CHECK(std::holds_alternative<algorithm_trainer::ValidationError>(validation));
  CHECK(repository.create_count == 0);
  CHECK(judge.call_count == 0);
}
