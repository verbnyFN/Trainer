#include "algorithm-trainer/sqlite-submission-repository.h"

#include "algorithm-trainer/judge.h"
#include "algorithm-trainer/submission-record.h"

#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace algorithm_trainer {
namespace {

constexpr int busy_timeout_milliseconds{5000};
constexpr int id_column{0};
constexpr int problem_id_column{1};
constexpr int language_column{2};
constexpr int source_code_column{3};
constexpr int status_column{4};
constexpr int verdict_column{5};
constexpr int created_at_column{6};
constexpr int completed_at_column{7};
constexpr int user_id_column{8};

class Statement {
public:
  Statement(sqlite3 *database, std::string_view sql) {
    if (sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()), &statement_,
                           nullptr) != SQLITE_OK) {
      error_ = sqlite3_errmsg(database);
    }
  }

  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;
  ~Statement() { sqlite3_finalize(statement_); }

  [[nodiscard]] sqlite3_stmt *get() const { return statement_; }
  [[nodiscard]] bool valid() const { return statement_ != nullptr; }
  [[nodiscard]] const std::string &error() const { return error_; }

private:
  sqlite3_stmt *statement_{};
  std::string error_;
};

RepositoryError database_error(sqlite3 *database, std::string_view operation) {
  return RepositoryError{std::string{operation} + ": " + sqlite3_errmsg(database)};
}

std::optional<RepositoryError> execute(sqlite3 *database, std::string_view sql) {
  char *message{};
  if (sqlite3_exec(database, std::string{sql}.c_str(), nullptr, nullptr, &message) == SQLITE_OK) {
    return std::nullopt;
  }
  std::string detail = message == nullptr ? sqlite3_errmsg(database) : message;
  sqlite3_free(message);
  return RepositoryError{"SQLite operation failed: " + detail};
}

std::variant<std::string, RepositoryError> read_migration(std::string_view file_name) {
  const auto path = std::filesystem::path{ALGORITHM_TRAINER_MIGRATIONS_DIR} / file_name;
  std::ifstream input{path};
  if (!input) {
    return RepositoryError{"Submission migration file is unavailable"};
  }
  return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::optional<RepositoryError> apply_migration(sqlite3 *database, int version,
                                               std::string_view file_name) {
  if (const auto error =
          execute(database,
                  "CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY);")) {
    return error;
  }

  Statement check{database, "SELECT 1 FROM schema_migrations WHERE version = ?;"};
  if (!check.valid()) {
    return RepositoryError{check.error()};
  }
  sqlite3_bind_int(check.get(), 1, version);
  if (sqlite3_step(check.get()) == SQLITE_ROW) {
    return std::nullopt;
  }

  auto migration = read_migration(file_name);
  if (const auto *error = std::get_if<RepositoryError>(&migration)) {
    return *error;
  }

  if (const auto error = execute(database, "BEGIN IMMEDIATE;")) {
    return error;
  }
  if (const auto error = execute(database, std::get<std::string>(migration))) {
    static_cast<void>(execute(database, "ROLLBACK;"));
    return error;
  }

  Statement record{database, "INSERT INTO schema_migrations(version) VALUES (?);"};
  if (!record.valid()) {
    static_cast<void>(execute(database, "ROLLBACK;"));
    return RepositoryError{record.error()};
  }
  sqlite3_bind_int(record.get(), 1, version);
  if (sqlite3_step(record.get()) != SQLITE_DONE) {
    auto error = database_error(database, "Could not record migration");
    static_cast<void>(execute(database, "ROLLBACK;"));
    return error;
  }
  return execute(database, "COMMIT;");
}

std::optional<Verdict> parse_verdict(std::string_view value) {
  if (value == "Accepted") {
    return Verdict::accepted;
  }
  if (value == "Wrong Answer") {
    return Verdict::wrong_answer;
  }
  if (value == "Runtime Error") {
    return Verdict::runtime_error;
  }
  if (value == "Time Limit Exceeded") {
    return Verdict::time_limit_exceeded;
  }
  return std::nullopt;
}

std::optional<SubmissionStatus> parse_status(std::string_view value) {
  if (value == "pending") {
    return SubmissionStatus::pending;
  }
  if (value == "completed") {
    return SubmissionStatus::completed;
  }
  if (value == "failed") {
    return SubmissionStatus::failed;
  }
  return std::nullopt;
}

std::string column_text(sqlite3_stmt *statement, int column) {
  const auto *text = sqlite3_column_text(statement, column);
  const auto size = sqlite3_column_bytes(statement, column);
  return text == nullptr
             ? std::string{}
             : std::string{reinterpret_cast<const char *>(text), static_cast<std::size_t>(size)};
}

void bind_text(sqlite3_stmt *statement, int parameter, const std::string &value) {
  sqlite3_bind_text(statement, parameter, value.data(), static_cast<int>(value.size()),
                    SQLITE_TRANSIENT);
}

std::variant<SubmissionRecord, RepositoryError> read_record(sqlite3_stmt *statement) {
  const auto status = parse_status(column_text(statement, status_column));
  if (!status) {
    return RepositoryError{"Database contains an unknown submission status"};
  }

  std::optional<Verdict> verdict;
  if (sqlite3_column_type(statement, verdict_column) != SQLITE_NULL) {
    verdict = parse_verdict(column_text(statement, verdict_column));
    if (!verdict) {
      return RepositoryError{"Database contains an unknown submission verdict"};
    }
  }

  std::optional<std::string> completed_at;
  if (sqlite3_column_type(statement, completed_at_column) != SQLITE_NULL) {
    completed_at = column_text(statement, completed_at_column);
  }

  return SubmissionRecord{
      .id = SubmissionId{sqlite3_column_int64(statement, id_column)},
      .problem_id = column_text(statement, problem_id_column),
      .language = column_text(statement, language_column),
      .source_code = column_text(statement, source_code_column),
      .status = *status,
      .verdict = verdict,
      .created_at = column_text(statement, created_at_column),
      .completed_at = std::move(completed_at),
      .user_id = sqlite3_column_type(statement, user_id_column) == SQLITE_NULL
                     ? std::nullopt
                     : std::optional<std::int64_t>{sqlite3_column_int64(statement, user_id_column)},
  };
}

constexpr std::string_view returned_columns{
    "id, problem_id, language, source_code, status, verdict, created_at, completed_at, user_id"};

} // namespace

struct SQLiteSubmissionRepository::Implementation {
  sqlite3 *database{};

  ~Implementation() { sqlite3_close(database); }
};

SQLiteSubmissionRepository::SQLiteSubmissionRepository(
    std::unique_ptr<Implementation> implementation)
    : implementation_{std::move(implementation)} {}

SQLiteSubmissionRepository::~SQLiteSubmissionRepository() = default;

SQLiteSubmissionRepository::OpenResult
SQLiteSubmissionRepository::open(const std::filesystem::path &database_path) {
  if (database_path != ":memory:" && database_path.has_parent_path()) {
    std::error_code error;
    std::filesystem::create_directories(database_path.parent_path(), error);
    if (error) {
      return RepositoryError{"Could not create database directory: " + error.message()};
    }
  }

  sqlite3 *database{};
  const auto result =
      sqlite3_open_v2(database_path.c_str(), &database,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
  if (result != SQLITE_OK) {
    const std::string message =
        database == nullptr ? "unknown SQLite error" : sqlite3_errmsg(database);
    sqlite3_close(database);
    return RepositoryError{"Could not open submission database: " + message};
  }

  auto implementation = std::make_unique<Implementation>();
  implementation->database = database;
  sqlite3_busy_timeout(database, busy_timeout_milliseconds);
  if (const auto error = execute(database, "PRAGMA foreign_keys = ON;")) {
    return *error;
  }
  if (const auto error = apply_migration(database, 1, "001-create-submissions.sql")) {
    return *error;
  }
  if (const auto error = apply_migration(database, 2, "002-create-auth.sql")) {
    return *error;
  }
  if (const auto error = apply_migration(database, 3, "003-link-submissions-to-users.sql")) {
    return *error;
  }
  return std::unique_ptr<SQLiteSubmissionRepository>{
      new SQLiteSubmissionRepository{std::move(implementation)}};
}

StoreSubmissionResult SQLiteSubmissionRepository::create(const SubmissionRequest &request) {
  const auto sql = "INSERT INTO submissions(problem_id, language, source_code, status, created_at, "
                   "user_id) VALUES (?, ?, ?, 'pending', "
                   "strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), ?) "
                   "RETURNING " +
                   std::string{returned_columns} + ';';
  Statement statement{implementation_->database, sql};
  if (!statement.valid()) {
    return RepositoryError{statement.error()};
  }
  bind_text(statement.get(), 1, request.problem_id);
  bind_text(statement.get(), 2, request.language);
  bind_text(statement.get(), 3, request.code);
  if (request.user_id) {
    sqlite3_bind_int64(statement.get(), 4, *request.user_id);
  } else {
    sqlite3_bind_null(statement.get(), 4);
  }
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    return database_error(implementation_->database, "Could not create submission");
  }
  return read_record(statement.get());
}

StoreSubmissionResult SQLiteSubmissionRepository::complete(SubmissionId submission_id,
                                                           Verdict verdict) {
  const auto sql = "UPDATE submissions SET status = 'completed', verdict = ?, "
                   "completed_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
                   "WHERE id = ? AND status = 'pending' RETURNING " +
                   std::string{returned_columns} + ';';
  Statement statement{implementation_->database, sql};
  if (!statement.valid()) {
    return RepositoryError{statement.error()};
  }
  const auto name = verdict_name(verdict);
  sqlite3_bind_text(statement.get(), 1, name.data(), static_cast<int>(name.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_int64(statement.get(), 2, submission_id.value);
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    return RepositoryError{"Submission is missing or is not pending"};
  }
  return read_record(statement.get());
}

StoreSubmissionResult SQLiteSubmissionRepository::fail(SubmissionId submission_id) {
  const auto sql = "UPDATE submissions SET status = 'failed', "
                   "completed_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
                   "WHERE id = ? AND status = 'pending' RETURNING " +
                   std::string{returned_columns} + ';';
  Statement statement{implementation_->database, sql};
  if (!statement.valid()) {
    return RepositoryError{statement.error()};
  }
  sqlite3_bind_int64(statement.get(), 1, submission_id.value);
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    return RepositoryError{"Submission is missing or is not pending"};
  }
  return read_record(statement.get());
}

FindSubmissionResult SQLiteSubmissionRepository::find(SubmissionId submission_id) {
  const auto sql = "SELECT " + std::string{returned_columns} + " FROM submissions WHERE id = ?;";
  Statement statement{implementation_->database, sql};
  if (!statement.valid()) {
    return RepositoryError{statement.error()};
  }
  sqlite3_bind_int64(statement.get(), 1, submission_id.value);
  const auto result = sqlite3_step(statement.get());
  if (result == SQLITE_DONE) {
    return std::optional<SubmissionRecord>{};
  }
  if (result != SQLITE_ROW) {
    return database_error(implementation_->database, "Could not retrieve submission");
  }
  auto record = read_record(statement.get());
  if (const auto *error = std::get_if<RepositoryError>(&record)) {
    return *error;
  }
  return std::optional<SubmissionRecord>{std::get<SubmissionRecord>(std::move(record))};
}

SubmissionHistoryResult SQLiteSubmissionRepository::history(std::int64_t user_id,
                                                            const std::string &problem_id) {
  const auto sql = "SELECT " + std::string{returned_columns} +
                   " FROM submissions WHERE user_id = ? AND problem_id = ? "
                   "ORDER BY created_at DESC, id DESC;";
  Statement statement{implementation_->database, sql};
  if (!statement.valid()) {
    return RepositoryError{statement.error()};
  }
  sqlite3_bind_int64(statement.get(), 1, user_id);
  bind_text(statement.get(), 2, problem_id);

  std::vector<SubmissionRecord> records;
  while (true) {
    const auto result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) {
      return records;
    }
    if (result != SQLITE_ROW) {
      return database_error(implementation_->database, "Could not retrieve submission history");
    }
    auto record = read_record(statement.get());
    if (const auto *error = std::get_if<RepositoryError>(&record)) {
      return *error;
    }
    records.push_back(std::get<SubmissionRecord>(std::move(record)));
  }
}

} // namespace algorithm_trainer
