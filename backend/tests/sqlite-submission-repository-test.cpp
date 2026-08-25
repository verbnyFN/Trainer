#include "algorithm-trainer/judge.h"
#include "algorithm-trainer/sqlite-submission-repository.h"
#include "algorithm-trainer/submission-record.h"
#include "algorithm-trainer/submission-repository.h"
#include "algorithm-trainer/submission.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    std::array<char, 64> path_template{};
    constexpr std::string_view pattern{"/tmp/algorithm-trainer-db-XXXXXX"};
    std::ranges::copy(pattern, path_template.begin());
    const auto *created = ::mkdtemp(path_template.data());
    REQUIRE(created != nullptr);
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] std::filesystem::path database() const { return path_ / "test.sqlite3"; }

private:
  std::filesystem::path path_;
};

std::unique_ptr<algorithm_trainer::SQLiteSubmissionRepository>
open_repository(const std::filesystem::path &path) {
  auto result = algorithm_trainer::SQLiteSubmissionRepository::open(path);
  REQUIRE(std::holds_alternative<std::unique_ptr<algorithm_trainer::SQLiteSubmissionRepository>>(
      result));
  return std::get<std::unique_ptr<algorithm_trainer::SQLiteSubmissionRepository>>(
      std::move(result));
}

algorithm_trainer::SubmissionRequest submission(std::string code = "print(3)") {
  return {
      .problem_id = "a-plus-b",
      .language = "python",
      .code = std::move(code),
  };
}

algorithm_trainer::SubmissionRecord stored_record(algorithm_trainer::StoreSubmissionResult result) {
  REQUIRE(std::holds_alternative<algorithm_trainer::SubmissionRecord>(result));
  return std::get<algorithm_trainer::SubmissionRecord>(std::move(result));
}

} // namespace

TEST_CASE("SQLite creates and completes a submission", "[sqlite]") {
  TemporaryDirectory directory;
  auto repository = open_repository(directory.database());

  const auto pending = stored_record(repository->create(submission()));
  const auto completed =
      stored_record(repository->complete(pending.id, algorithm_trainer::Verdict::accepted));

  CHECK(pending.id.value > 0);
  CHECK(pending.status == algorithm_trainer::SubmissionStatus::pending);
  CHECK_FALSE(pending.verdict.has_value());
  CHECK(completed.status == algorithm_trainer::SubmissionStatus::completed);
  CHECK(completed.verdict == algorithm_trainer::Verdict::accepted);
  CHECK(completed.completed_at.has_value());
}

TEST_CASE("SQLite persists submissions across repository instances", "[sqlite]") {
  TemporaryDirectory directory;
  algorithm_trainer::SubmissionId id;
  {
    auto repository = open_repository(directory.database());
    id = stored_record(repository->create(submission("print('persisted')"))).id;
  }

  auto reopened = open_repository(directory.database());
  const auto result = reopened->find(id);

  REQUIRE(std::holds_alternative<std::optional<algorithm_trainer::SubmissionRecord>>(result));
  const auto &record = std::get<std::optional<algorithm_trainer::SubmissionRecord>>(result);
  REQUIRE(record.has_value());
  CHECK(record->source_code == "print('persisted')");
}

TEST_CASE("SQLite persists every public verdict", "[sqlite]") {
  TemporaryDirectory directory;
  auto repository = open_repository(directory.database());
  constexpr std::array verdicts{
      algorithm_trainer::Verdict::accepted,
      algorithm_trainer::Verdict::wrong_answer,
      algorithm_trainer::Verdict::runtime_error,
      algorithm_trainer::Verdict::time_limit_exceeded,
  };

  for (const auto verdict : verdicts) {
    const auto pending = stored_record(repository->create(submission()));
    const auto completed = stored_record(repository->complete(pending.id, verdict));
    CHECK(completed.verdict == verdict);
  }
}

TEST_CASE("SQLite prepared statements preserve hostile-looking source", "[sqlite]") {
  TemporaryDirectory directory;
  auto repository = open_repository(directory.database());
  std::string source{"'); DROP TABLE submissions; --\nprint(3)"};
  source.push_back('\0');
  source.append("binary");

  const auto first = stored_record(repository->create(submission(source)));
  const auto second = stored_record(repository->create(submission("print(4)")));
  const auto found = repository->find(first.id);

  CHECK(second.id.value > first.id.value);
  REQUIRE(std::holds_alternative<std::optional<algorithm_trainer::SubmissionRecord>>(found));
  const auto &record = std::get<std::optional<algorithm_trainer::SubmissionRecord>>(found);
  REQUIRE(record.has_value());
  CHECK(record->source_code == source);
}

TEST_CASE("SQLite records failed judging and prevents repeated transitions", "[sqlite]") {
  TemporaryDirectory directory;
  auto repository = open_repository(directory.database());
  const auto pending = stored_record(repository->create(submission()));

  const auto failed = stored_record(repository->fail(pending.id));
  const auto repeated = repository->complete(pending.id, algorithm_trainer::Verdict::accepted);

  CHECK(failed.status == algorithm_trainer::SubmissionStatus::failed);
  CHECK_FALSE(failed.verdict.has_value());
  CHECK(failed.completed_at.has_value());
  CHECK(std::holds_alternative<algorithm_trainer::RepositoryError>(repeated));
}

TEST_CASE("SQLite returns an empty result for unknown submission ids", "[sqlite]") {
  TemporaryDirectory directory;
  auto repository = open_repository(directory.database());

  const auto result = repository->find({999});

  REQUIRE(std::holds_alternative<std::optional<algorithm_trainer::SubmissionRecord>>(result));
  CHECK_FALSE(std::get<std::optional<algorithm_trainer::SubmissionRecord>>(result).has_value());
}
