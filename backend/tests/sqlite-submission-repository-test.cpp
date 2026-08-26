#include "algorithm-trainer/auth-service.h"
#include "algorithm-trainer/judge.h"
#include "algorithm-trainer/sqlite-submission-repository.h"
#include "algorithm-trainer/submission-record.h"
#include "algorithm-trainer/submission-repository.h"
#include "algorithm-trainer/submission.h"

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

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
#include <vector>

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

void set_completion_day(const std::filesystem::path &database,
                        algorithm_trainer::SubmissionId submission_id,
                        std::string_view day_modifier) {
  sqlite3 *connection{};
  REQUIRE(sqlite3_open(database.c_str(), &connection) == SQLITE_OK);

  sqlite3_stmt *statement{};
  REQUIRE(sqlite3_prepare_v2(connection,
                             "UPDATE submissions SET completed_at = "
                             "strftime('%Y-%m-%dT12:00:00.000Z', 'now', ?) WHERE id = ?;",
                             -1, &statement, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_bind_text(statement, 1, day_modifier.data(),
                            static_cast<int>(day_modifier.size()), SQLITE_TRANSIENT) == SQLITE_OK);
  REQUIRE(sqlite3_bind_int64(statement, 2, submission_id.value) == SQLITE_OK);
  REQUIRE(sqlite3_step(statement) == SQLITE_DONE);
  sqlite3_finalize(statement);
  sqlite3_close(connection);
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

TEST_CASE("SQLite persists normalized runtime error types", "[sqlite]") {
  TemporaryDirectory directory;
  auto repository = open_repository(directory.database());
  const auto pending = stored_record(repository->create(submission()));

  const auto completed = stored_record(
      repository->complete(pending.id, algorithm_trainer::Verdict::runtime_error, "Syntax Error"));
  const auto found = repository->find(pending.id);

  CHECK(completed.error_type == "Syntax Error");
  REQUIRE(std::holds_alternative<std::optional<algorithm_trainer::SubmissionRecord>>(found));
  REQUIRE(std::get<std::optional<algorithm_trainer::SubmissionRecord>>(found).has_value());
  CHECK(std::get<std::optional<algorithm_trainer::SubmissionRecord>>(found)->error_type ==
        "Syntax Error");
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

TEST_CASE("SQLite submission history is isolated by user and problem", "[sqlite]") {
  TemporaryDirectory directory;
  auto first_auth_result = algorithm_trainer::AuthService::open(directory.database());
  REQUIRE(
      std::holds_alternative<std::unique_ptr<algorithm_trainer::AuthService>>(first_auth_result));
  auto auth_service =
      std::get<std::unique_ptr<algorithm_trainer::AuthService>>(std::move(first_auth_result));
  const auto first_registration = auth_service->register_user("history-user", "password-one");
  const auto second_registration = auth_service->register_user("other-user", "password-two");
  REQUIRE(std::holds_alternative<algorithm_trainer::AuthSession>(first_registration));
  REQUIRE(std::holds_alternative<algorithm_trainer::AuthSession>(second_registration));
  const auto first_user = std::get<algorithm_trainer::AuthSession>(first_registration).user.id;
  const auto second_user = std::get<algorithm_trainer::AuthSession>(second_registration).user.id;
  auto repository = open_repository(directory.database());

  auto first_request = submission("print('first')");
  first_request.user_id = first_user;
  auto other_problem = submission("print('other problem')");
  other_problem.problem_id = "different-problem";
  other_problem.user_id = first_user;
  auto other_user = submission("print('other user')");
  other_user.user_id = second_user;
  static_cast<void>(stored_record(repository->create(first_request)));
  static_cast<void>(stored_record(repository->create(other_problem)));
  static_cast<void>(stored_record(repository->create(other_user)));

  const auto history = repository->history(first_user, "a-plus-b");

  REQUIRE(std::holds_alternative<std::vector<algorithm_trainer::SubmissionRecord>>(history));
  const auto &records = std::get<std::vector<algorithm_trainer::SubmissionRecord>>(history);
  REQUIRE(records.size() == 1);
  CHECK(records.front().source_code == "print('first')");
  CHECK(records.front().user_id == first_user);
}

TEST_CASE("SQLite derives user progress from accepted submissions", "[sqlite]") {
  TemporaryDirectory directory;
  auto auth_result = algorithm_trainer::AuthService::open(directory.database());
  REQUIRE(std::holds_alternative<std::unique_ptr<algorithm_trainer::AuthService>>(auth_result));
  auto auth_service =
      std::get<std::unique_ptr<algorithm_trainer::AuthService>>(std::move(auth_result));
  const auto registration = auth_service->register_user("profile-user", "password-one");
  REQUIRE(std::holds_alternative<algorithm_trainer::AuthSession>(registration));
  const auto user_id = std::get<algorithm_trainer::AuthSession>(registration).user.id;
  auto repository = open_repository(directory.database());

  auto first = submission("print('accepted once')");
  first.user_id = user_id;
  auto second = submission("print('accepted twice')");
  second.user_id = user_id;
  auto wrong = submission("print('wrong')");
  wrong.user_id = user_id;
  auto other_problem = submission("print('other')");
  other_problem.problem_id = "other-problem";
  other_problem.user_id = user_id;
  const auto first_record = stored_record(repository->create(first));
  const auto second_record = stored_record(repository->create(second));
  const auto wrong_record = stored_record(repository->create(wrong));
  const auto other_record = stored_record(repository->create(other_problem));
  static_cast<void>(
      stored_record(repository->complete(first_record.id, algorithm_trainer::Verdict::accepted)));
  static_cast<void>(
      stored_record(repository->complete(second_record.id, algorithm_trainer::Verdict::accepted)));
  static_cast<void>(stored_record(
      repository->complete(wrong_record.id, algorithm_trainer::Verdict::wrong_answer)));
  static_cast<void>(
      stored_record(repository->complete(other_record.id, algorithm_trainer::Verdict::accepted)));

  set_completion_day(directory.database(), first_record.id, "-2 days");
  set_completion_day(directory.database(), second_record.id, "-1 day");
  set_completion_day(directory.database(), other_record.id, "0 days");

  const auto result = repository->progress(user_id);

  REQUIRE(std::holds_alternative<algorithm_trainer::UserProgress>(result));
  const auto &progress = std::get<algorithm_trainer::UserProgress>(result);
  CHECK(progress.total_submissions == 4);
  CHECK(progress.accepted_submissions == 3);
  CHECK(progress.current_streak_days == 3);
  REQUIRE(progress.completed_problems.size() == 2);
  CHECK(std::ranges::any_of(progress.completed_problems, [](const auto &completed) {
    return completed.problem_id == "a-plus-b";
  }));

  set_completion_day(directory.database(), first_record.id, "-5 days");
  set_completion_day(directory.database(), second_record.id, "-4 days");
  set_completion_day(directory.database(), other_record.id, "-3 days");
  const auto stale_result = repository->progress(user_id);
  REQUIRE(std::holds_alternative<algorithm_trainer::UserProgress>(stale_result));
  CHECK(std::get<algorithm_trainer::UserProgress>(stale_result).current_streak_days == 0);
}
