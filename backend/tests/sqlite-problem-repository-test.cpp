#include "algorithm-trainer/sqlite-problem-repository.h"
#include "algorithm-trainer/sqlite-submission-repository.h"
#include "algorithm-trainer/submission-record.h"

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <ranges>
#include <string_view>
#include <system_error>
#include <variant>

namespace {

class TemporaryDatabase {
public:
  TemporaryDatabase() {
    std::array<char, 64> path_template{};
    constexpr std::string_view pattern{"/tmp/algorithm-trainer-problems-XXXXXX"};
    std::ranges::copy(pattern, path_template.begin());
    const auto *created = ::mkdtemp(path_template.data());
    REQUIRE(created != nullptr);
    directory_ = created;
  }

  ~TemporaryDatabase() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  [[nodiscard]] std::filesystem::path path() const { return directory_ / "test.sqlite3"; }

private:
  std::filesystem::path directory_;
};

std::unique_ptr<algorithm_trainer::SQLiteSubmissionRepository>
open_submissions(const std::filesystem::path &path) {
  auto result = algorithm_trainer::SQLiteSubmissionRepository::open(path);
  REQUIRE(std::holds_alternative<std::unique_ptr<algorithm_trainer::SQLiteSubmissionRepository>>(
      result));
  return std::get<std::unique_ptr<algorithm_trainer::SQLiteSubmissionRepository>>(
      std::move(result));
}

std::unique_ptr<algorithm_trainer::SQLiteProblemRepository>
open_problems(const std::filesystem::path &path) {
  auto result = algorithm_trainer::SQLiteProblemRepository::open(path);
  REQUIRE(
      std::holds_alternative<std::unique_ptr<algorithm_trainer::SQLiteProblemRepository>>(result));
  return std::get<std::unique_ptr<algorithm_trainer::SQLiteProblemRepository>>(std::move(result));
}

algorithm_trainer::Problem require_problem(algorithm_trainer::ProblemFindResult result) {
  REQUIRE(std::holds_alternative<std::optional<algorithm_trainer::Problem>>(result));
  auto problem = std::get<std::optional<algorithm_trainer::Problem>>(std::move(result));
  REQUIRE(problem.has_value());
  return std::move(*problem);
}

void execute(const std::filesystem::path &path, std::string_view sql) {
  sqlite3 *database{};
  REQUIRE(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
  REQUIRE(sqlite3_exec(database, sql.data(), nullptr, nullptr, nullptr) == SQLITE_OK);
  sqlite3_close(database);
}

} // namespace

TEST_CASE("SQLite problems persist metadata without exposing hidden tests", "[problem][sqlite]") {
  TemporaryDatabase database;
  auto submissions = open_submissions(database.path());
  auto problems = open_problems(database.path());

  auto public_problem = require_problem(problems->find_enabled("a-plus-b"));
  CHECK(public_problem.id == "a-plus-b");
  CHECK(public_problem.hidden_tests.empty());

  auto list = problems->list_enabled();
  REQUIRE(std::holds_alternative<std::vector<algorithm_trainer::Problem>>(list));
  for (const auto &problem : std::get<std::vector<algorithm_trainer::Problem>>(list))
    CHECK(problem.hidden_tests.empty());

  problems.reset();
  execute(database.path(), "UPDATE problems SET title = 'Persistent A + B' "
                           "WHERE id = 'a-plus-b';");
  problems = open_problems(database.path());
  CHECK(require_problem(problems->find_enabled("a-plus-b")).title == "Persistent A + B");
}

TEST_CASE("judge lookup loads only enabled hidden tests in stored order", "[problem][sqlite]") {
  TemporaryDatabase database;
  auto submissions = open_submissions(database.path());
  auto problems = open_problems(database.path());

  execute(
      database.path(),
      "UPDATE problem_tests SET position = position + 100 WHERE problem_id = 'a-plus-b';"
      "UPDATE problem_tests SET enabled = 0 WHERE problem_id = 'a-plus-b' AND position = 100;"
      "UPDATE problem_tests SET position = 0 WHERE problem_id = 'a-plus-b' AND position = 101;");

  const auto judged = require_problem(problems->find_for_judging("a-plus-b"));
  REQUIRE_FALSE(judged.hidden_tests.empty());
  CHECK(judged.hidden_tests.front().input == "0 0\n");
  CHECK(judged.hidden_tests.size() == 9);
}

TEST_CASE("disabled problems are hidden publicly but remain available to historical judging",
          "[problem][sqlite]") {
  TemporaryDatabase database;
  auto submissions = open_submissions(database.path());
  auto problems = open_problems(database.path());
  execute(database.path(), "UPDATE problems SET enabled = 0 WHERE id = 'a-plus-b';");

  auto public_result = problems->find_enabled("a-plus-b");
  REQUIRE(std::holds_alternative<std::optional<algorithm_trainer::Problem>>(public_result));
  CHECK_FALSE(std::get<std::optional<algorithm_trainer::Problem>>(public_result).has_value());
  CHECK(require_problem(problems->find_for_judging("a-plus-b")).enabled == false);
}

TEST_CASE("changing hidden tests does not alter stored submission verdicts", "[problem][sqlite]") {
  TemporaryDatabase database;
  auto submissions = open_submissions(database.path());
  auto problems = open_problems(database.path());
  algorithm_trainer::SubmissionRequest request{
      .problem_id = "a-plus-b", .language = "cpp", .code = "stored source"};
  auto created = submissions->create(request);
  REQUIRE(std::holds_alternative<algorithm_trainer::SubmissionRecord>(created));
  auto claimed = submissions->claim_next();
  REQUIRE(std::holds_alternative<std::optional<algorithm_trainer::SubmissionRecord>>(claimed));
  const auto running =
      std::get<std::optional<algorithm_trainer::SubmissionRecord>>(std::move(claimed));
  REQUIRE(running.has_value());
  auto completed = submissions->complete(running->id, algorithm_trainer::Verdict::accepted);
  REQUIRE(std::holds_alternative<algorithm_trainer::SubmissionRecord>(completed));

  execute(database.path(), "UPDATE problem_tests SET expected_output = 'changed' "
                           "WHERE problem_id = 'a-plus-b';");
  auto stored = submissions->find(running->id);
  REQUIRE(std::holds_alternative<std::optional<algorithm_trainer::SubmissionRecord>>(stored));
  const auto record =
      std::get<std::optional<algorithm_trainer::SubmissionRecord>>(std::move(stored));
  REQUIRE(record.has_value());
  CHECK(record->verdict == algorithm_trainer::Verdict::accepted);
}

TEST_CASE("admin problem and hidden-test CRUD remains separated from public lookup",
          "[problem][sqlite][admin]") {
  TemporaryDatabase database;
  auto submissions = open_submissions(database.path());
  auto problems = open_problems(database.path());

  algorithm_trainer::Problem created{.id = "admin-created",
                                     .title = "Admin Created",
                                     .description = "Statement",
                                     .input_format = "Input",
                                     .output_format = "Output",
                                     .difficulty = algorithm_trainer::ProblemDifficulty::hard,
                                     .tags = {"admin"},
                                     .languages = {"python"},
                                     .examples = {{"1\n", "1\n"}},
                                     .enabled = false};
  REQUIRE(std::holds_alternative<algorithm_trainer::Problem>(problems->create(created)));
  CHECK(
      std::holds_alternative<algorithm_trainer::ProblemRepositoryError>(problems->create(created)));
  CHECK_FALSE(require_problem(problems->find_any(created.id)).enabled);
  auto public_lookup = problems->find_enabled(created.id);
  REQUIRE(std::holds_alternative<std::optional<algorithm_trainer::Problem>>(public_lookup));
  CHECK_FALSE(std::get<std::optional<algorithm_trainer::Problem>>(public_lookup));

  algorithm_trainer::ProblemTestCase test{.input = "secret input\n",
                                          .expected_output = "secret output\n",
                                          .position = 7,
                                          .enabled = false};
  auto test_result = problems->create_test(created.id, test);
  REQUIRE(std::holds_alternative<algorithm_trainer::ProblemTestCase>(test_result));
  test = std::get<algorithm_trainer::ProblemTestCase>(test_result);
  REQUIRE(test.id > 0);

  const auto admin_problem = require_problem(problems->find_any(created.id));
  REQUIRE(admin_problem.hidden_tests.size() == 1);
  CHECK(admin_problem.hidden_tests.front().input == "secret input\n");
  CHECK_FALSE(admin_problem.hidden_tests.front().enabled);
  CHECK(require_problem(problems->find_for_judging(created.id)).hidden_tests.empty());

  test.enabled = true;
  test.position = 2;
  auto stale_test = test;
  auto updated_test = problems->update_test(created.id, test);
  REQUIRE(std::holds_alternative<algorithm_trainer::ProblemTestCase>(updated_test));
  test = std::get<algorithm_trainer::ProblemTestCase>(updated_test);
  stale_test.input = "stale overwrite";
  CHECK(std::holds_alternative<algorithm_trainer::ProblemRepositoryError>(
      problems->update_test(created.id, stale_test)));
  CHECK(require_problem(problems->find_for_judging(created.id)).hidden_tests.size() == 1);
  REQUIRE(std::get<bool>(problems->remove_test(created.id, test.id)));
  REQUIRE(std::get<bool>(problems->remove(created.id)));
  CHECK_FALSE(std::get<std::optional<algorithm_trainer::Problem>>(problems->find_any(created.id)));
}

TEST_CASE("public problem JSON never serializes hidden tests", "[problem][security]") {
  algorithm_trainer::Problem problem{
      .id = "safe", .title = "Safe", .hidden_tests = {{"do not expose", "also secret"}}};
  const auto public_json = algorithm_trainer::problem_to_json(problem);
  const auto summary_json = algorithm_trainer::problem_summary_to_json(problem);
  CHECK_FALSE(public_json.isMember("tests"));
  CHECK_FALSE(public_json.isMember("hiddenTests"));
  CHECK_FALSE(summary_json.isMember("tests"));
  CHECK(public_json.toStyledString().find("do not expose") == std::string::npos);
}

TEST_CASE("stale problem revisions cannot overwrite a concurrent admin edit",
          "[problem][sqlite][admin]") {
  TemporaryDatabase database;
  auto submissions = open_submissions(database.path());
  auto problems = open_problems(database.path());
  auto first = require_problem(problems->find_any("a-plus-b"));
  auto stale = first;
  first.title = "First editor";
  auto updated = problems->update(first);
  REQUIRE(std::holds_alternative<algorithm_trainer::Problem>(updated));
  CHECK(std::get<algorithm_trainer::Problem>(updated).revision == first.revision + 1);
  stale.title = "Stale editor";
  auto conflict = problems->update(stale);
  REQUIRE(std::holds_alternative<algorithm_trainer::ProblemRepositoryError>(conflict));
  CHECK(require_problem(problems->find_any("a-plus-b")).title == "First editor");
}
