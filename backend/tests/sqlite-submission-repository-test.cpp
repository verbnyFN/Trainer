#include "algorithm-trainer/auth-service.h"
#include "algorithm-trainer/judge.h"
#include "algorithm-trainer/sqlite-submission-repository.h"
#include "algorithm-trainer/submission-record.h"
#include "algorithm-trainer/submission-repository.h"
#include "algorithm-trainer/submission.h"

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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
open_repository(const std::filesystem::path &path,
                algorithm_trainer::SubmissionQueueLimits limits = {}) {
  auto result = algorithm_trainer::SQLiteSubmissionRepository::open(path, limits);
  REQUIRE(std::holds_alternative<std::unique_ptr<algorithm_trainer::SQLiteSubmissionRepository>>(
      result));
  return std::get<std::unique_ptr<algorithm_trainer::SQLiteSubmissionRepository>>(
      std::move(result));
}

algorithm_trainer::RepositoryError
repository_error(algorithm_trainer::StoreSubmissionResult result) {
  REQUIRE(std::holds_alternative<algorithm_trainer::RepositoryError>(result));
  return std::get<algorithm_trainer::RepositoryError>(std::move(result));
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

algorithm_trainer::SubmissionRecord
claim_record(algorithm_trainer::SubmissionRepository &repository) {
  auto result = repository.claim_next();
  REQUIRE(std::holds_alternative<std::optional<algorithm_trainer::SubmissionRecord>>(result));
  auto claimed = std::get<std::optional<algorithm_trainer::SubmissionRecord>>(std::move(result));
  REQUIRE(claimed.has_value());
  return std::move(*claimed);
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
  const auto running = claim_record(*repository);
  const auto completed =
      stored_record(repository->complete(running.id, algorithm_trainer::Verdict::accepted));

  CHECK(pending.id.value > 0);
  CHECK(pending.status == algorithm_trainer::SubmissionStatus::queued);
  CHECK(running.status == algorithm_trainer::SubmissionStatus::running);
  CHECK_FALSE(pending.verdict.has_value());
  CHECK(completed.status == algorithm_trainer::SubmissionStatus::completed);
  CHECK(completed.verdict == algorithm_trainer::Verdict::accepted);
  CHECK(completed.completed_at.has_value());
}

TEST_CASE("SQLite rejects submissions when the global queue is full", "[sqlite][queue-limit]") {
  TemporaryDirectory directory;
  auto repository =
      open_repository(directory.database(), {
                                                .maximum_active_submissions = 2,
                                                .maximum_active_submissions_per_user = 5,
                                                .maximum_running_submissions_per_user = 1,
                                            });
  static_cast<void>(stored_record(repository->create(submission("first"))));
  static_cast<void>(stored_record(repository->create(submission("second"))));

  const auto error = repository_error(repository->create(submission("rejected")));

  CHECK(error.code == algorithm_trainer::RepositoryErrorCode::queue_full);
  CHECK(error.message == "Submission queue is full");
}

TEST_CASE("Concurrent queue admission cannot exceed the global limit", "[sqlite][queue-limit]") {
  TemporaryDirectory directory;
  auto repository =
      open_repository(directory.database(), {
                                                .maximum_active_submissions = 8,
                                                .maximum_active_submissions_per_user = 100,
                                                .maximum_running_submissions_per_user = 1,
                                            });
  std::atomic<int> accepted{};
  std::atomic<int> full{};
  std::atomic<int> unexpected{};
  std::vector<std::thread> requests;
  for (int index = 0; index < 32; ++index) {
    requests.emplace_back([&, index] {
      auto result = repository->create(submission("concurrent-" + std::to_string(index)));
      if (std::holds_alternative<algorithm_trainer::SubmissionRecord>(result)) {
        ++accepted;
      } else if (std::get<algorithm_trainer::RepositoryError>(result).code ==
                 algorithm_trainer::RepositoryErrorCode::queue_full) {
        ++full;
      } else {
        ++unexpected;
      }
    });
  }
  for (auto &request : requests) {
    request.join();
  }

  CHECK(accepted == 8);
  CHECK(full == 24);
  CHECK(unexpected == 0);
}

TEST_CASE("SQLite limits active submissions per user without blocking other users",
          "[sqlite][queue-limit]") {
  TemporaryDirectory directory;
  auto auth_result = algorithm_trainer::AuthService::open(directory.database());
  REQUIRE(std::holds_alternative<std::unique_ptr<algorithm_trainer::AuthService>>(auth_result));
  auto auth = std::get<std::unique_ptr<algorithm_trainer::AuthService>>(std::move(auth_result));
  const auto first_registration = auth->register_user("limited-user", "password-one");
  const auto second_registration = auth->register_user("other-limited-user", "password-two");
  REQUIRE(std::holds_alternative<algorithm_trainer::AuthSession>(first_registration));
  REQUIRE(std::holds_alternative<algorithm_trainer::AuthSession>(second_registration));
  const auto first_user = std::get<algorithm_trainer::AuthSession>(first_registration).user.id;
  const auto second_user = std::get<algorithm_trainer::AuthSession>(second_registration).user.id;
  auto repository =
      open_repository(directory.database(), {
                                                .maximum_active_submissions = 10,
                                                .maximum_active_submissions_per_user = 1,
                                                .maximum_running_submissions_per_user = 1,
                                            });
  auto first = submission("first user");
  first.user_id = first_user;
  auto repeated = submission("first user again");
  repeated.user_id = first_user;
  auto other = submission("other user");
  other.user_id = second_user;
  static_cast<void>(stored_record(repository->create(first)));

  const auto limited = repository_error(repository->create(repeated));
  const auto accepted = repository->create(other);

  CHECK(limited.code == algorithm_trainer::RepositoryErrorCode::user_submission_limit);
  CHECK(limited.message == "Too many active submissions for this user");
  CHECK(std::holds_alternative<algorithm_trainer::SubmissionRecord>(accepted));
}

TEST_CASE("SQLite claims work fairly across users", "[sqlite][queue-fairness]") {
  TemporaryDirectory directory;
  auto auth_result = algorithm_trainer::AuthService::open(directory.database());
  REQUIRE(std::holds_alternative<std::unique_ptr<algorithm_trainer::AuthService>>(auth_result));
  auto auth = std::get<std::unique_ptr<algorithm_trainer::AuthService>>(std::move(auth_result));
  const auto first_registration = auth->register_user("fair-user", "password-one");
  const auto second_registration = auth->register_user("fair-other", "password-two");
  REQUIRE(std::holds_alternative<algorithm_trainer::AuthSession>(first_registration));
  REQUIRE(std::holds_alternative<algorithm_trainer::AuthSession>(second_registration));
  const auto first_user = std::get<algorithm_trainer::AuthSession>(first_registration).user.id;
  const auto second_user = std::get<algorithm_trainer::AuthSession>(second_registration).user.id;
  auto repository = open_repository(directory.database());
  auto first = submission("first");
  first.user_id = first_user;
  auto second = submission("second");
  second.user_id = first_user;
  auto other = submission("other");
  other.user_id = second_user;
  const auto first_record = stored_record(repository->create(first));
  const auto second_record = stored_record(repository->create(second));
  const auto other_record = stored_record(repository->create(other));

  const auto first_claim = claim_record(*repository);
  const auto second_claim = claim_record(*repository);
  const auto blocked_claim = repository->claim_next();

  CHECK(first_claim.id == first_record.id);
  CHECK(second_claim.id == other_record.id);
  REQUIRE(
      std::holds_alternative<std::optional<algorithm_trainer::SubmissionRecord>>(blocked_claim));
  CHECK_FALSE(
      std::get<std::optional<algorithm_trainer::SubmissionRecord>>(blocked_claim).has_value());
  static_cast<void>(
      stored_record(repository->complete(first_claim.id, algorithm_trainer::Verdict::accepted)));
  CHECK(claim_record(*repository).id == second_record.id);
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
    const auto running = claim_record(*repository);
    CHECK(running.id == pending.id);
    const auto completed = stored_record(repository->complete(running.id, verdict));
    CHECK(completed.verdict == verdict);
  }
}

TEST_CASE("SQLite persists normalized runtime error types", "[sqlite]") {
  TemporaryDirectory directory;
  auto repository = open_repository(directory.database());
  const auto pending = stored_record(repository->create(submission()));
  const auto running = claim_record(*repository);

  const auto completed = stored_record(
      repository->complete(running.id, algorithm_trainer::Verdict::runtime_error, "Syntax Error"));
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
  const auto running = claim_record(*repository);

  const auto failed = stored_record(repository->fail(running.id));
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
  const auto queued_progress_result = repository->progress(user_id);
  REQUIRE(std::holds_alternative<algorithm_trainer::UserProgress>(queued_progress_result));
  const auto &queued_progress = std::get<algorithm_trainer::UserProgress>(queued_progress_result);
  CHECK(queued_progress.total_submissions == 4);
  CHECK(queued_progress.accepted_submissions == 0);
  CHECK(queued_progress.completed_problems.empty());

  CHECK(claim_record(*repository).id == first_record.id);
  static_cast<void>(
      stored_record(repository->complete(first_record.id, algorithm_trainer::Verdict::accepted)));
  CHECK(claim_record(*repository).id == second_record.id);
  static_cast<void>(
      stored_record(repository->complete(second_record.id, algorithm_trainer::Verdict::accepted)));
  CHECK(claim_record(*repository).id == wrong_record.id);
  static_cast<void>(stored_record(
      repository->complete(wrong_record.id, algorithm_trainer::Verdict::wrong_answer)));
  CHECK(claim_record(*repository).id == other_record.id);
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
  CHECK(progress.most_recent_submission_problem_id == "other-problem");
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

TEST_CASE("admin retry is atomic, unique, and audited", "[sqlite][admin][retry]") {
  TemporaryDirectory directory;
  auto auth_result = algorithm_trainer::AuthService::open(directory.database());
  REQUIRE(std::holds_alternative<std::unique_ptr<algorithm_trainer::AuthService>>(auth_result));
  auto auth = std::get<std::unique_ptr<algorithm_trainer::AuthService>>(std::move(auth_result));
  auto admin_result = auth->ensure_admin("retry-admin", "password-one");
  REQUIRE(std::holds_alternative<algorithm_trainer::AuthUser>(admin_result));
  const auto admin_id = std::get<algorithm_trainer::AuthUser>(admin_result).id;

  auto first_repository = open_repository(directory.database());
  const auto original = stored_record(first_repository->create(submission("raise RuntimeError")));
  CHECK(claim_record(*first_repository).id == original.id);
  static_cast<void>(stored_record(first_repository->fail(original.id, "runtime-error")));
  auto second_repository = open_repository(directory.database());

  std::atomic<int> successful_retries{};
  const auto retry = [&](algorithm_trainer::SQLiteSubmissionRepository &repository) {
    auto result = repository.retry(original.id, admin_id);
    if (std::holds_alternative<algorithm_trainer::SubmissionRecord>(result))
      ++successful_retries;
  };
  std::thread first{retry, std::ref(*first_repository)};
  std::thread second{retry, std::ref(*second_repository)};
  first.join();
  second.join();
  CHECK(successful_retries == 1);

  sqlite3 *database{};
  REQUIRE(sqlite3_open(directory.database().c_str(), &database) == SQLITE_OK);
  sqlite3_stmt *statement{};
  REQUIRE(sqlite3_prepare_v2(database,
                             "SELECT (SELECT COUNT(*) FROM submissions WHERE retry_of = ?), "
                             "(SELECT COUNT(*) FROM admin_audit_log WHERE action = "
                             "'submission.retry' AND entity_id = ?);",
                             -1, &statement, nullptr) == SQLITE_OK);
  sqlite3_bind_int64(statement, 1, original.id.value);
  const auto id = std::to_string(original.id.value);
  sqlite3_bind_text(statement, 2, id.c_str(), -1, SQLITE_TRANSIENT);
  REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
  CHECK(sqlite3_column_int(statement, 0) == 1);
  CHECK(sqlite3_column_int(statement, 1) == 1);
  sqlite3_finalize(statement);
  sqlite3_close(database);
}
