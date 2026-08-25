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

  algorithm_trainer::StoreSubmissionResult complete(algorithm_trainer::SubmissionId,
                                                    algorithm_trainer::Verdict verdict) override {
    ++complete_count;
    completed_verdict = verdict;
    record.status = algorithm_trainer::SubmissionStatus::completed;
    record.verdict = verdict;
    record.completed_at = "2026-01-01T00:00:01.000Z";
    return record;
  }

  algorithm_trainer::StoreSubmissionResult fail(algorithm_trainer::SubmissionId) override {
    ++fail_count;
    record.status = algorithm_trainer::SubmissionStatus::failed;
    record.completed_at = "2026-01-01T00:00:01.000Z";
    return record;
  }

  algorithm_trainer::FindSubmissionResult find(algorithm_trainer::SubmissionId id) override {
    if (id == record.id) {
      return std::optional{record};
    }
    return std::optional<algorithm_trainer::SubmissionRecord>{};
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

TEST_CASE("invalid submissions never reach persistence or judging", "[submission-service]") {
  RecordingJudge judge;
  RecordingRepository repository;
  algorithm_trainer::SubmissionService service{judge, repository};
  auto json = valid_json();

  SECTION("missing code") { json.removeMember("code"); }
  SECTION("unsupported problem") { json["problemId"] = "another-problem"; }
  SECTION("unsupported language") { json["language"] = "cpp"; }
  SECTION("empty code") { json["code"] = ""; }

  const auto validation = algorithm_trainer::validate_submission(json);
  if (const auto *submission = std::get_if<algorithm_trainer::SubmissionRequest>(&validation)) {
    service.submit(*submission);
  }

  CHECK(std::holds_alternative<algorithm_trainer::ValidationError>(validation));
  CHECK(repository.create_count == 0);
  CHECK(judge.call_count == 0);
}
