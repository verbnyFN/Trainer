#include "algorithm-trainer/sqlite-problem-repository.h"

#include "algorithm-trainer/problem-seed.h"

#include <json/json.h>
#include <sqlite3.h>

#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace algorithm_trainer {
namespace {

class Statement {
public:
  Statement(sqlite3 *database, std::string_view sql) {
    sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()), &statement_, nullptr);
  }
  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement &) = delete;
  [[nodiscard]] sqlite3_stmt *get() const { return statement_; }
  [[nodiscard]] bool valid() const { return statement_ != nullptr; }

private:
  sqlite3_stmt *statement_{};
};

void bind_text(sqlite3_stmt *statement, int index, const std::string &value) {
  sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()),
                    SQLITE_TRANSIENT);
}

std::string column_text(sqlite3_stmt *statement, int index) {
  const auto *value = sqlite3_column_text(statement, index);
  return value == nullptr ? std::string{} : reinterpret_cast<const char *>(value);
}

std::string json_text(const Json::Value &value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, value);
}

Json::Value strings_json(const std::vector<std::string> &values) {
  Json::Value json{Json::arrayValue};
  for (const auto &value : values) {
    json.append(value);
  }
  return json;
}

Json::Value examples_json(const std::vector<ProblemExample> &examples) {
  Json::Value json{Json::arrayValue};
  for (const auto &example : examples) {
    Json::Value item;
    item["input"] = example.input;
    item["output"] = example.output;
    json.append(std::move(item));
  }
  return json;
}

std::optional<Json::Value> parse_json(const std::string &text) {
  Json::CharReaderBuilder builder;
  Json::Value value;
  std::string errors;
  std::istringstream input{text};
  if (!Json::parseFromStream(builder, input, &value, &errors)) {
    return std::nullopt;
  }
  return value;
}

std::optional<ProblemDifficulty> parse_difficulty(std::string_view value) {
  if (value == "Easy")
    return ProblemDifficulty::easy;
  if (value == "Medium")
    return ProblemDifficulty::medium;
  if (value == "Hard")
    return ProblemDifficulty::hard;
  return std::nullopt;
}

std::variant<Problem, ProblemRepositoryError> read_problem(sqlite3 *database,
                                                           sqlite3_stmt *statement,
                                                           bool include_tests,
                                                           bool include_disabled_tests = false) {
  const auto difficulty = parse_difficulty(column_text(statement, 5));
  const auto tags = parse_json(column_text(statement, 6));
  const auto languages = parse_json(column_text(statement, 7));
  const auto examples = parse_json(column_text(statement, 8));
  if (!difficulty || !tags || !tags->isArray() || !languages || !languages->isArray() ||
      !examples || !examples->isArray()) {
    return ProblemRepositoryError{"Database contains invalid problem metadata"};
  }
  Problem problem{
      .id = column_text(statement, 0),
      .title = column_text(statement, 1),
      .description = column_text(statement, 2),
      .input_format = column_text(statement, 3),
      .output_format = column_text(statement, 4),
      .difficulty = *difficulty,
      .enabled = sqlite3_column_int(statement, 9) != 0,
      .revision = sqlite3_column_int64(statement, 10),
  };
  for (const auto &tag : *tags)
    problem.tags.push_back(tag.asString());
  for (const auto &language : *languages)
    problem.languages.push_back(language.asString());
  for (const auto &example : *examples) {
    if (!example.isObject() || !example["input"].isString() || !example["output"].isString()) {
      return ProblemRepositoryError{"Database contains invalid problem examples"};
    }
    problem.examples.push_back({example["input"].asString(), example["output"].asString()});
  }
  if (!include_tests)
    return problem;

  Statement tests{
      database,
      include_disabled_tests
          ? "SELECT input, expected_output, id, position, enabled, revision FROM problem_tests "
            "WHERE problem_id = ? ORDER BY position, id;"
          : "SELECT input, expected_output, id, position, enabled, revision FROM problem_tests "
            "WHERE problem_id = ? AND enabled = 1 ORDER BY position, id;"};
  if (!tests.valid())
    return ProblemRepositoryError{"Could not prepare hidden-test lookup"};
  bind_text(tests.get(), 1, problem.id);
  while (true) {
    const auto result = sqlite3_step(tests.get());
    if (result == SQLITE_DONE)
      break;
    if (result != SQLITE_ROW)
      return ProblemRepositoryError{"Could not retrieve hidden tests"};
    problem.hidden_tests.push_back({.input = column_text(tests.get(), 0),
                                    .expected_output = column_text(tests.get(), 1),
                                    .id = sqlite3_column_int64(tests.get(), 2),
                                    .position = sqlite3_column_int64(tests.get(), 3),
                                    .enabled = sqlite3_column_int(tests.get(), 4) != 0,
                                    .revision = sqlite3_column_int64(tests.get(), 5)});
  }
  return problem;
}

std::optional<ProblemRepositoryError> seed(sqlite3 *database) {
  if (sqlite3_exec(database, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK)
    return ProblemRepositoryError{"Could not begin initial problem transaction"};
  const auto rollback = [database]() {
    sqlite3_exec(database, "ROLLBACK;", nullptr, nullptr, nullptr);
  };
  Statement insert_problem{
      database,
      "INSERT OR IGNORE INTO problems(id, title, description, input_format, output_format, "
      "difficulty, tags_json, languages_json, examples_json, enabled, created_at, updated_at) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 1, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
      "strftime('%Y-%m-%dT%H:%M:%fZ', 'now'));"};
  Statement insert_test{database,
                        "INSERT INTO problem_tests(problem_id, input, expected_output, position, "
                        "enabled) VALUES (?, ?, ?, ?, 1);"};
  if (!insert_problem.valid() || !insert_test.valid()) {
    rollback();
    return ProblemRepositoryError{"Could not prepare initial problem data"};
  }
  for (const auto &problem : default_problems()) {
    sqlite3_reset(insert_problem.get());
    sqlite3_clear_bindings(insert_problem.get());
    bind_text(insert_problem.get(), 1, problem.id);
    bind_text(insert_problem.get(), 2, problem.title);
    bind_text(insert_problem.get(), 3, problem.description);
    bind_text(insert_problem.get(), 4, problem.input_format);
    bind_text(insert_problem.get(), 5, problem.output_format);
    bind_text(insert_problem.get(), 6, std::string{difficulty_name(problem.difficulty)});
    bind_text(insert_problem.get(), 7, json_text(strings_json(problem.tags)));
    bind_text(insert_problem.get(), 8, json_text(strings_json(problem.languages)));
    bind_text(insert_problem.get(), 9, json_text(examples_json(problem.examples)));
    if (sqlite3_step(insert_problem.get()) != SQLITE_DONE) {
      rollback();
      return ProblemRepositoryError{"Could not seed initial problem"};
    }
    if (sqlite3_changes(database) == 0)
      continue;
    for (std::size_t index = 0; index < problem.hidden_tests.size(); ++index) {
      sqlite3_reset(insert_test.get());
      sqlite3_clear_bindings(insert_test.get());
      bind_text(insert_test.get(), 1, problem.id);
      bind_text(insert_test.get(), 2, problem.hidden_tests[index].input);
      bind_text(insert_test.get(), 3, problem.hidden_tests[index].expected_output);
      sqlite3_bind_int64(insert_test.get(), 4, static_cast<sqlite3_int64>(index));
      if (sqlite3_step(insert_test.get()) != SQLITE_DONE) {
        rollback();
        return ProblemRepositoryError{"Could not seed initial hidden tests"};
      }
    }
  }
  if (sqlite3_exec(database, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
    rollback();
    return ProblemRepositoryError{"Could not commit initial problem data"};
  }
  return std::nullopt;
}

} // namespace

struct SQLiteProblemRepository::Implementation {
  sqlite3 *database{};
  ~Implementation() { sqlite3_close(database); }
};

SQLiteProblemRepository::SQLiteProblemRepository(std::unique_ptr<Implementation> implementation)
    : implementation_{std::move(implementation)} {}
SQLiteProblemRepository::~SQLiteProblemRepository() = default;

SQLiteProblemRepository::OpenResult
SQLiteProblemRepository::open(const std::filesystem::path &database_path) {
  sqlite3 *database{};
  if (sqlite3_open_v2(database_path.c_str(), &database,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
    const std::string message =
        database == nullptr ? "unknown SQLite error" : sqlite3_errmsg(database);
    sqlite3_close(database);
    return ProblemRepositoryError{"Could not open problem database: " + message};
  }
  sqlite3_busy_timeout(database, 5000);
  sqlite3_exec(database, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
  if (const auto error = seed(database)) {
    sqlite3_close(database);
    return *error;
  }
  auto implementation = std::make_unique<Implementation>();
  implementation->database = database;
  return std::unique_ptr<SQLiteProblemRepository>{
      new SQLiteProblemRepository{std::move(implementation)}};
}

ProblemListResult SQLiteProblemRepository::list_enabled() {
  Statement statement{implementation_->database,
                      "SELECT id, title, description, input_format, output_format, difficulty, "
                      "tags_json, languages_json, examples_json, enabled, revision FROM problems "
                      "WHERE enabled = 1 ORDER BY rowid;"};
  if (!statement.valid())
    return ProblemRepositoryError{"Could not prepare problem list"};
  std::vector<Problem> problems;
  while (true) {
    const auto result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE)
      break;
    if (result != SQLITE_ROW)
      return ProblemRepositoryError{"Could not retrieve problem list"};
    auto problem = read_problem(implementation_->database, statement.get(), false);
    if (const auto *error = std::get_if<ProblemRepositoryError>(&problem))
      return *error;
    problems.push_back(std::get<Problem>(std::move(problem)));
  }
  return problems;
}

ProblemFindResult SQLiteProblemRepository::find_enabled(const std::string &id) {
  Statement statement{implementation_->database,
                      "SELECT id, title, description, input_format, output_format, difficulty, "
                      "tags_json, languages_json, examples_json, enabled, revision FROM problems "
                      "WHERE id = ? AND enabled = 1;"};
  if (!statement.valid())
    return ProblemRepositoryError{"Could not prepare problem lookup"};
  bind_text(statement.get(), 1, id);
  const auto result = sqlite3_step(statement.get());
  if (result == SQLITE_DONE)
    return std::optional<Problem>{};
  if (result != SQLITE_ROW)
    return ProblemRepositoryError{"Could not retrieve problem"};
  auto problem = read_problem(implementation_->database, statement.get(), false);
  if (const auto *error = std::get_if<ProblemRepositoryError>(&problem))
    return *error;
  return std::optional<Problem>{std::get<Problem>(std::move(problem))};
}

ProblemFindResult SQLiteProblemRepository::find_for_judging(const std::string &id) {
  Statement statement{
      implementation_->database,
      "SELECT id, title, description, input_format, output_format, difficulty, "
      "tags_json, languages_json, examples_json, enabled, revision FROM problems WHERE id = ?;"};
  if (!statement.valid())
    return ProblemRepositoryError{"Could not prepare judge problem lookup"};
  bind_text(statement.get(), 1, id);
  const auto result = sqlite3_step(statement.get());
  if (result == SQLITE_DONE)
    return std::optional<Problem>{};
  if (result != SQLITE_ROW)
    return ProblemRepositoryError{"Could not retrieve judge problem"};
  auto problem = read_problem(implementation_->database, statement.get(), true);
  if (const auto *error = std::get_if<ProblemRepositoryError>(&problem))
    return *error;
  return std::optional<Problem>{std::get<Problem>(std::move(problem))};
}

ProblemListResult SQLiteProblemRepository::list_all() {
  Statement statement{implementation_->database,
                      "SELECT id, title, description, input_format, output_format, difficulty, "
                      "tags_json, languages_json, examples_json, enabled, revision FROM problems "
                      "ORDER BY rowid;"};
  if (!statement.valid())
    return ProblemRepositoryError{"Could not prepare admin problem list"};
  std::vector<Problem> problems;
  while (true) {
    const auto result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE)
      return problems;
    if (result != SQLITE_ROW)
      return ProblemRepositoryError{"Could not retrieve admin problem list"};
    auto problem = read_problem(implementation_->database, statement.get(), false);
    if (const auto *error = std::get_if<ProblemRepositoryError>(&problem))
      return *error;
    problems.push_back(std::get<Problem>(std::move(problem)));
  }
}

ProblemFindResult SQLiteProblemRepository::find_any(const std::string &id) {
  Statement statement{
      implementation_->database,
      "SELECT id, title, description, input_format, output_format, difficulty, "
      "tags_json, languages_json, examples_json, enabled, revision FROM problems WHERE id = ?;"};
  if (!statement.valid())
    return ProblemRepositoryError{"Could not prepare admin problem lookup"};
  bind_text(statement.get(), 1, id);
  const auto result = sqlite3_step(statement.get());
  if (result == SQLITE_DONE)
    return std::optional<Problem>{};
  if (result != SQLITE_ROW)
    return ProblemRepositoryError{"Could not retrieve admin problem"};
  auto problem = read_problem(implementation_->database, statement.get(), true, true);
  if (const auto *error = std::get_if<ProblemRepositoryError>(&problem))
    return *error;
  return std::optional<Problem>{std::get<Problem>(std::move(problem))};
}

ProblemWriteResult SQLiteProblemRepository::create(const Problem &problem) {
  Statement statement{
      implementation_->database,
      "INSERT INTO problems(id, title, description, input_format, output_format, difficulty, "
      "tags_json, languages_json, examples_json, enabled, created_at, updated_at) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
      "strftime('%Y-%m-%dT%H:%M:%fZ', 'now'));"};
  if (!statement.valid())
    return ProblemRepositoryError{"Could not prepare problem creation"};
  bind_text(statement.get(), 1, problem.id);
  bind_text(statement.get(), 2, problem.title);
  bind_text(statement.get(), 3, problem.description);
  bind_text(statement.get(), 4, problem.input_format);
  bind_text(statement.get(), 5, problem.output_format);
  bind_text(statement.get(), 6, std::string{difficulty_name(problem.difficulty)});
  bind_text(statement.get(), 7, json_text(strings_json(problem.tags)));
  bind_text(statement.get(), 8, json_text(strings_json(problem.languages)));
  bind_text(statement.get(), 9, json_text(examples_json(problem.examples)));
  sqlite3_bind_int(statement.get(), 10, problem.enabled ? 1 : 0);
  if (sqlite3_step(statement.get()) != SQLITE_DONE)
    return ProblemRepositoryError{"Problem could not be created"};
  return problem;
}

ProblemWriteResult SQLiteProblemRepository::update(const Problem &problem) {
  Statement statement{
      implementation_->database,
      "UPDATE problems SET title = ?, description = ?, input_format = ?, output_format = ?, "
      "difficulty = ?, tags_json = ?, languages_json = ?, examples_json = ?, enabled = ?, "
      "revision = revision + 1, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
      "WHERE id = ? AND revision = ?;"};
  if (!statement.valid())
    return ProblemRepositoryError{"Could not prepare problem update"};
  bind_text(statement.get(), 1, problem.title);
  bind_text(statement.get(), 2, problem.description);
  bind_text(statement.get(), 3, problem.input_format);
  bind_text(statement.get(), 4, problem.output_format);
  bind_text(statement.get(), 5, std::string{difficulty_name(problem.difficulty)});
  bind_text(statement.get(), 6, json_text(strings_json(problem.tags)));
  bind_text(statement.get(), 7, json_text(strings_json(problem.languages)));
  bind_text(statement.get(), 8, json_text(examples_json(problem.examples)));
  sqlite3_bind_int(statement.get(), 9, problem.enabled ? 1 : 0);
  bind_text(statement.get(), 10, problem.id);
  sqlite3_bind_int64(statement.get(), 11, problem.revision);
  if (sqlite3_step(statement.get()) != SQLITE_DONE)
    return ProblemRepositoryError{"Problem could not be updated"};
  if (sqlite3_changes(implementation_->database) == 0)
    return ProblemRepositoryError{"Problem was changed by another administrator"};
  auto updated = problem;
  ++updated.revision;
  return updated;
}

ProblemDeleteResult SQLiteProblemRepository::remove(const std::string &id) {
  Statement statement{implementation_->database, "DELETE FROM problems WHERE id = ?;"};
  if (!statement.valid())
    return ProblemRepositoryError{"Could not prepare problem deletion"};
  bind_text(statement.get(), 1, id);
  if (sqlite3_step(statement.get()) != SQLITE_DONE)
    return ProblemRepositoryError{"Problem could not be deleted"};
  return sqlite3_changes(implementation_->database) != 0;
}

ProblemTestWriteResult SQLiteProblemRepository::create_test(const std::string &problem_id,
                                                            const ProblemTestCase &test) {
  Statement statement{
      implementation_->database,
      "INSERT INTO problem_tests(problem_id, input, expected_output, position, enabled) "
      "VALUES (?, ?, ?, ?, ?) RETURNING id;"};
  if (!statement.valid())
    return ProblemRepositoryError{"Could not prepare hidden-test creation"};
  bind_text(statement.get(), 1, problem_id);
  bind_text(statement.get(), 2, test.input);
  bind_text(statement.get(), 3, test.expected_output);
  sqlite3_bind_int64(statement.get(), 4, test.position);
  sqlite3_bind_int(statement.get(), 5, test.enabled ? 1 : 0);
  if (sqlite3_step(statement.get()) != SQLITE_ROW)
    return ProblemRepositoryError{"Hidden test could not be created"};
  auto created = test;
  created.id = sqlite3_column_int64(statement.get(), 0);
  return created;
}

ProblemTestWriteResult SQLiteProblemRepository::update_test(const std::string &problem_id,
                                                            const ProblemTestCase &test) {
  Statement statement{implementation_->database,
                      "UPDATE problem_tests SET input = ?, expected_output = ?, position = ?, "
                      "enabled = ?, revision = revision + 1 WHERE id = ? AND problem_id = ? "
                      "AND revision = ?;"};
  if (!statement.valid())
    return ProblemRepositoryError{"Could not prepare hidden-test update"};
  bind_text(statement.get(), 1, test.input);
  bind_text(statement.get(), 2, test.expected_output);
  sqlite3_bind_int64(statement.get(), 3, test.position);
  sqlite3_bind_int(statement.get(), 4, test.enabled ? 1 : 0);
  sqlite3_bind_int64(statement.get(), 5, test.id);
  bind_text(statement.get(), 6, problem_id);
  sqlite3_bind_int64(statement.get(), 7, test.revision);
  if (sqlite3_step(statement.get()) != SQLITE_DONE)
    return ProblemRepositoryError{"Hidden test could not be updated"};
  if (sqlite3_changes(implementation_->database) == 0)
    return ProblemRepositoryError{"Hidden test was changed by another administrator"};
  auto updated = test;
  ++updated.revision;
  return updated;
}

ProblemDeleteResult SQLiteProblemRepository::remove_test(const std::string &problem_id,
                                                         std::int64_t test_id) {
  Statement statement{implementation_->database,
                      "DELETE FROM problem_tests WHERE id = ? AND problem_id = ?;"};
  if (!statement.valid())
    return ProblemRepositoryError{"Could not prepare hidden-test deletion"};
  sqlite3_bind_int64(statement.get(), 1, test_id);
  bind_text(statement.get(), 2, problem_id);
  if (sqlite3_step(statement.get()) != SQLITE_DONE)
    return ProblemRepositoryError{"Hidden test could not be deleted"};
  return sqlite3_changes(implementation_->database) != 0;
}

} // namespace algorithm_trainer
