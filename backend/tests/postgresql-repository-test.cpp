#include "algorithm-trainer/auth-service.h"
#include "algorithm-trainer/postgresql-problem-repository.h"
#include "algorithm-trainer/postgresql-submission-repository.h"
#include "postgresql-client.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <future>
#include <memory>
#include <string>

TEST_CASE("PostgreSQL persistence supports the complete application flow", "[postgresql]") {
  const auto *configured = std::getenv("ALGORITHM_TRAINER_TEST_DATABASE_URL");
  if (configured == nullptr)
    SKIP("ALGORITHM_TRAINER_TEST_DATABASE_URL is not configured");
  const std::string connection_info{configured};
  algorithm_trainer::postgresql::Connection guard{connection_info};
  REQUIRE(guard.valid());
  auto database_name = guard.execute("SELECT current_database()");
  REQUIRE(database_name.tuples_ok());
  REQUIRE(database_name.value(0, 0).ends_with("_test"));
  REQUIRE_FALSE(algorithm_trainer::postgresql::apply_migrations(connection_info).has_value());
  auto cleared = guard.execute(
      "TRUNCATE admin_audit_log, sessions, submissions, problem_tests, problems, users "
      "RESTART IDENTITY CASCADE");
  REQUIRE(cleared.command_ok());

  auto auth_result = algorithm_trainer::PostgreSQLAuthService::open(connection_info);
  REQUIRE(std::holds_alternative<std::unique_ptr<algorithm_trainer::PostgreSQLAuthService>>(
      auth_result));
  auto auth =
      std::get<std::unique_ptr<algorithm_trainer::PostgreSQLAuthService>>(std::move(auth_result));
  auto session_result = auth->register_user("postgres-user", "postgres-password");
  REQUIRE(std::holds_alternative<algorithm_trainer::AuthSession>(session_result));
  const auto session = std::get<algorithm_trainer::AuthSession>(session_result);
  CHECK(std::holds_alternative<algorithm_trainer::AuthUser>(auth->current_user(session.token)));

  auto problems_result = algorithm_trainer::PostgreSQLProblemRepository::open(connection_info);
  REQUIRE(std::holds_alternative<std::unique_ptr<algorithm_trainer::PostgreSQLProblemRepository>>(
      problems_result));
  auto problems = std::get<std::unique_ptr<algorithm_trainer::PostgreSQLProblemRepository>>(
      std::move(problems_result));
  auto problem = problems->find_for_judging("a-plus-b");
  REQUIRE(std::holds_alternative<std::optional<algorithm_trainer::Problem>>(problem));
  REQUIRE(std::get<std::optional<algorithm_trainer::Problem>>(problem).has_value());
  CHECK_FALSE(std::get<std::optional<algorithm_trainer::Problem>>(problem)->hidden_tests.empty());

  auto submissions_result =
      algorithm_trainer::PostgreSQLSubmissionRepository::open(connection_info);
  REQUIRE(
      std::holds_alternative<std::unique_ptr<algorithm_trainer::PostgreSQLSubmissionRepository>>(
          submissions_result));
  auto submissions = std::get<std::unique_ptr<algorithm_trainer::PostgreSQLSubmissionRepository>>(
      std::move(submissions_result));
  auto created = submissions->create({.problem_id = "a-plus-b",
                                      .language = "python",
                                      .code = "print(3)",
                                      .user_id = session.user.id});
  REQUIRE(std::holds_alternative<algorithm_trainer::SubmissionRecord>(created));
  const auto id = std::get<algorithm_trainer::SubmissionRecord>(created).id;
  auto claimed = submissions->claim_next();
  REQUIRE(std::holds_alternative<std::optional<algorithm_trainer::SubmissionRecord>>(claimed));
  REQUIRE(std::get<std::optional<algorithm_trainer::SubmissionRecord>>(claimed).has_value());
  auto completed = submissions->complete(id, algorithm_trainer::Verdict::accepted, std::nullopt);
  REQUIRE(std::holds_alternative<algorithm_trainer::SubmissionRecord>(completed));
  auto progress = submissions->progress(session.user.id);
  REQUIRE(std::holds_alternative<algorithm_trainer::UserProgress>(progress));
  CHECK(std::get<algorithm_trainer::UserProgress>(progress).accepted_submissions == 1);

  REQUIRE(std::holds_alternative<algorithm_trainer::SubmissionRecord>(
      submissions->create({.problem_id = "a-plus-b",
                           .language = "python",
                           .code = "print(4)",
                           .user_id = session.user.id})));
  REQUIRE(std::holds_alternative<algorithm_trainer::SubmissionRecord>(
      submissions->create({.problem_id = "a-plus-b",
                           .language = "python",
                           .code = "print(5)",
                           .user_id = session.user.id})));
  auto second_result = algorithm_trainer::PostgreSQLSubmissionRepository::open(connection_info);
  REQUIRE(
      std::holds_alternative<std::unique_ptr<algorithm_trainer::PostgreSQLSubmissionRepository>>(
          second_result));
  auto second = std::get<std::unique_ptr<algorithm_trainer::PostgreSQLSubmissionRepository>>(
      std::move(second_result));
  auto first_claim = std::async(std::launch::async, [&] { return submissions->claim_next(); });
  auto second_claim = std::async(std::launch::async, [&] { return second->claim_next(); });
  const auto claimed_count = [](const algorithm_trainer::ClaimSubmissionResult &result) {
    const auto *record = std::get_if<std::optional<algorithm_trainer::SubmissionRecord>>(&result);
    return record != nullptr && record->has_value() ? 1 : 0;
  };
  CHECK(claimed_count(first_claim.get()) + claimed_count(second_claim.get()) == 1);
}
