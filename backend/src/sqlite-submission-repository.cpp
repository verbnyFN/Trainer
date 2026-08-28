#include "algorithm-trainer/sqlite-submission-repository.h"

#include "algorithm-trainer/judge.h"
#include "algorithm-trainer/submission-record.h"

#include <sqlite3.h>

#include <cctype>
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
constexpr int error_type_column{9};
constexpr int retry_of_column{10};

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
  if (value == "queued") {
    return SubmissionStatus::queued;
  }
  if (value == "running") {
    return SubmissionStatus::running;
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
      .error_type = sqlite3_column_type(statement, error_type_column) == SQLITE_NULL
                        ? std::nullopt
                        : std::optional<std::string>{column_text(statement, error_type_column)},
      .retry_of = sqlite3_column_type(statement, retry_of_column) == SQLITE_NULL
                      ? std::nullopt
                      : std::optional<SubmissionId>{SubmissionId{
                            sqlite3_column_int64(statement, retry_of_column)}},
  };
}

constexpr std::string_view returned_columns{
    "id, problem_id, language, source_code, status, verdict, created_at, completed_at, user_id, "
    "error_type, retry_of"};

} // namespace

struct SQLiteSubmissionRepository::Implementation {
  sqlite3 *database{};
  SubmissionQueueLimits limits;

  ~Implementation() { sqlite3_close(database); }
};

SQLiteSubmissionRepository::SQLiteSubmissionRepository(
    std::unique_ptr<Implementation> implementation)
    : implementation_{std::move(implementation)} {}

SQLiteSubmissionRepository::~SQLiteSubmissionRepository() = default;

SQLiteSubmissionRepository::OpenResult
SQLiteSubmissionRepository::open(const std::filesystem::path &database_path,
                                 SubmissionQueueLimits limits) {
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
  implementation->limits = limits;
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
  if (const auto error = apply_migration(database, 4, "004-add-submission-error-type.sql")) {
    return *error;
  }
  if (const auto error = apply_migration(database, 5, "005-add-submission-queue.sql")) {
    return *error;
  }
  if (const auto error = apply_migration(database, 6, "006-add-admin-users.sql")) {
    return *error;
  }
  if (const auto error = apply_migration(database, 7, "007-create-problems.sql")) {
    return *error;
  }
  if (const auto error = apply_migration(database, 8, "008-add-admin-operations.sql")) {
    return *error;
  }
  return std::unique_ptr<SQLiteSubmissionRepository>{
      new SQLiteSubmissionRepository{std::move(implementation)}};
}

StoreSubmissionResult SQLiteSubmissionRepository::create(const SubmissionRequest &request) {
  const auto sql = "INSERT INTO submissions(problem_id, language, source_code, status, created_at, "
                   "user_id) SELECT ?, ?, ?, 'queued', "
                   "strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), ? "
                   "WHERE (SELECT COUNT(*) FROM submissions "
                   "WHERE status IN ('queued', 'running')) < ? "
                   "AND (SELECT COUNT(*) FROM submissions WHERE status IN ('queued', 'running') "
                   "AND user_id IS ?) < ? "
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
    sqlite3_bind_int64(statement.get(), 6, *request.user_id);
  } else {
    sqlite3_bind_null(statement.get(), 4);
    sqlite3_bind_null(statement.get(), 6);
  }
  sqlite3_bind_int64(
      statement.get(), 5,
      static_cast<sqlite3_int64>(implementation_->limits.maximum_active_submissions));
  sqlite3_bind_int64(
      statement.get(), 7,
      static_cast<sqlite3_int64>(implementation_->limits.maximum_active_submissions_per_user));
  const auto result = sqlite3_step(statement.get());
  if (result == SQLITE_DONE) {
    Statement user_count{implementation_->database,
                         "SELECT COUNT(*) FROM submissions WHERE status IN ('queued', 'running') "
                         "AND user_id IS ?;"};
    if (!user_count.valid()) {
      return RepositoryError{user_count.error()};
    }
    if (request.user_id) {
      sqlite3_bind_int64(user_count.get(), 1, *request.user_id);
    } else {
      sqlite3_bind_null(user_count.get(), 1);
    }
    if (sqlite3_step(user_count.get()) == SQLITE_ROW &&
        static_cast<std::size_t>(sqlite3_column_int64(user_count.get(), 0)) >=
            implementation_->limits.maximum_active_submissions_per_user) {
      return RepositoryError{"Too many active submissions for this user",
                             RepositoryErrorCode::user_submission_limit};
    }
    return RepositoryError{"Submission queue is full", RepositoryErrorCode::queue_full};
  }
  if (result != SQLITE_ROW) {
    return database_error(implementation_->database, "Could not create submission");
  }
  return read_record(statement.get());
}

StoreSubmissionResult SQLiteSubmissionRepository::complete(SubmissionId submission_id,
                                                           Verdict verdict,
                                                           std::optional<std::string> error_type) {
  const auto sql = "UPDATE submissions SET status = 'completed', verdict = ?, "
                   "error_type = ?, completed_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
                   "WHERE id = ? AND status = 'running' RETURNING " +
                   std::string{returned_columns} + ';';
  Statement statement{implementation_->database, sql};
  if (!statement.valid()) {
    return RepositoryError{statement.error()};
  }
  const auto name = verdict_name(verdict);
  sqlite3_bind_text(statement.get(), 1, name.data(), static_cast<int>(name.size()),
                    SQLITE_TRANSIENT);
  if (error_type) {
    bind_text(statement.get(), 2, *error_type);
  } else {
    sqlite3_bind_null(statement.get(), 2);
  }
  sqlite3_bind_int64(statement.get(), 3, submission_id.value);
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    return RepositoryError{"Submission is missing or is not running"};
  }
  return read_record(statement.get());
}

StoreSubmissionResult SQLiteSubmissionRepository::fail(SubmissionId submission_id,
                                                       std::optional<std::string> error_type) {
  const auto sql = "UPDATE submissions SET status = 'failed', error_type = ?, "
                   "completed_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') "
                   "WHERE id = ? AND status = 'running' RETURNING " +
                   std::string{returned_columns} + ';';
  Statement statement{implementation_->database, sql};
  if (!statement.valid()) {
    return RepositoryError{statement.error()};
  }
  if (error_type) {
    bind_text(statement.get(), 1, *error_type);
  } else {
    sqlite3_bind_null(statement.get(), 1);
  }
  sqlite3_bind_int64(statement.get(), 2, submission_id.value);
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    return RepositoryError{"Submission is missing or is not running"};
  }
  return read_record(statement.get());
}

ClaimSubmissionResult SQLiteSubmissionRepository::claim_next() {
  const auto sql = "UPDATE submissions SET status = 'running' WHERE id = ("
                   "SELECT queued.id FROM submissions AS queued WHERE queued.status = 'queued' "
                   "AND (SELECT COUNT(*) FROM submissions AS running "
                   "WHERE running.status = 'running' AND running.user_id IS queued.user_id) < ? "
                   "ORDER BY queued.id LIMIT 1"
                   ") AND status = 'queued' RETURNING " +
                   std::string{returned_columns} + ';';
  Statement statement{implementation_->database, sql};
  if (!statement.valid()) {
    return RepositoryError{statement.error()};
  }
  sqlite3_bind_int64(
      statement.get(), 1,
      static_cast<sqlite3_int64>(implementation_->limits.maximum_running_submissions_per_user));
  const auto result = sqlite3_step(statement.get());
  if (result == SQLITE_DONE) {
    return std::optional<SubmissionRecord>{};
  }
  if (result != SQLITE_ROW) {
    return database_error(implementation_->database, "Could not claim queued submission");
  }
  auto record = read_record(statement.get());
  if (const auto *error = std::get_if<RepositoryError>(&record)) {
    return *error;
  }
  return std::optional<SubmissionRecord>{std::get<SubmissionRecord>(std::move(record))};
}

RecoverSubmissionsResult SQLiteSubmissionRepository::recover_running() {
  Statement statement{implementation_->database,
                      "UPDATE submissions SET status = 'queued' WHERE status = 'running';"};
  if (!statement.valid()) {
    return RepositoryError{statement.error()};
  }
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    return database_error(implementation_->database, "Could not recover running submissions");
  }
  return static_cast<std::size_t>(sqlite3_changes(implementation_->database));
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

SubmissionHistoryResult SQLiteSubmissionRepository::list_all() {
  const auto sql = "SELECT " + std::string{returned_columns} +
                   " FROM submissions ORDER BY created_at DESC, id DESC;";
  Statement statement{implementation_->database, sql};
  if (!statement.valid())
    return RepositoryError{statement.error()};
  std::vector<SubmissionRecord> records;
  while (true) {
    const auto result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE)
      return records;
    if (result != SQLITE_ROW)
      return database_error(implementation_->database, "Could not retrieve submissions");
    auto record = read_record(statement.get());
    if (const auto *error = std::get_if<RepositoryError>(&record))
      return *error;
    records.push_back(std::get<SubmissionRecord>(std::move(record)));
  }
}

SubmissionHistoryResult
SQLiteSubmissionRepository::list_filtered(const SubmissionAdminFilter &filter) {
  std::string sql = "SELECT " + std::string{returned_columns} + " FROM submissions WHERE 1 = 1";
  std::vector<std::variant<std::string, std::int64_t>> parameters;
  const auto add_text = [&](std::string_view clause, const std::optional<std::string> &value) {
    if (value) {
      sql += clause;
      parameters.emplace_back(*value);
    }
  };
  if (filter.status) {
    sql += " AND status = ?";
    parameters.emplace_back(std::string{submission_status_name(*filter.status)});
    std::get<std::string>(parameters.back())[0] =
        static_cast<char>(std::tolower(std::get<std::string>(parameters.back())[0]));
  }
  add_text(" AND error_type = ?", filter.error_type);
  add_text(" AND language = ?", filter.language);
  add_text(" AND problem_id = ?", filter.problem_id);
  if (filter.user_id) {
    sql += " AND user_id = ?";
    parameters.emplace_back(*filter.user_id);
  }
  add_text(" AND created_at >= ?", filter.created_from);
  add_text(" AND created_at <= ?", filter.created_to);
  sql += " ORDER BY created_at DESC, id DESC LIMIT 500;";
  Statement statement{implementation_->database, sql};
  if (!statement.valid())
    return RepositoryError{statement.error()};
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (const auto *text = std::get_if<std::string>(&parameters[index]))
      bind_text(statement.get(), static_cast<int>(index + 1), *text);
    else
      sqlite3_bind_int64(statement.get(), static_cast<int>(index + 1),
                         std::get<std::int64_t>(parameters[index]));
  }
  std::vector<SubmissionRecord> records;
  while (true) {
    const auto result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE)
      return records;
    if (result != SQLITE_ROW)
      return database_error(implementation_->database, "Could not filter submissions");
    auto record = read_record(statement.get());
    if (const auto *error = std::get_if<RepositoryError>(&record))
      return *error;
    records.push_back(std::get<SubmissionRecord>(std::move(record)));
  }
}

StoreSubmissionResult SQLiteSubmissionRepository::retry(SubmissionId submission_id,
                                                        std::int64_t admin_user_id) {
  if (const auto error = execute(implementation_->database, "BEGIN IMMEDIATE;"))
    return *error;
  const auto rollback = [this]() {
    static_cast<void>(execute(implementation_->database, "ROLLBACK;"));
  };
  const auto sql = "INSERT INTO submissions(problem_id, language, source_code, status, created_at, "
                   "user_id, retry_of) SELECT original.problem_id, original.language, "
                   "original.source_code, 'queued', strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
                   "original.user_id, original.id FROM submissions AS original "
                   "WHERE original.id = ? AND (original.status = 'failed' OR "
                   "original.verdict = 'Runtime Error') AND NOT EXISTS "
                   "(SELECT 1 FROM submissions WHERE retry_of = ?) AND "
                   "(SELECT COUNT(*) FROM submissions WHERE status IN ('queued', 'running')) < ? "
                   "AND (SELECT COUNT(*) FROM submissions WHERE status IN ('queued', 'running') "
                   "AND user_id IS original.user_id) < ? RETURNING " +
                   std::string{returned_columns} + ';';
  Statement statement{implementation_->database, sql};
  if (!statement.valid()) {
    rollback();
    return RepositoryError{statement.error()};
  }
  sqlite3_bind_int64(statement.get(), 1, submission_id.value);
  sqlite3_bind_int64(statement.get(), 2, submission_id.value);
  sqlite3_bind_int64(
      statement.get(), 3,
      static_cast<sqlite3_int64>(implementation_->limits.maximum_active_submissions));
  sqlite3_bind_int64(
      statement.get(), 4,
      static_cast<sqlite3_int64>(implementation_->limits.maximum_active_submissions_per_user));
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    rollback();
    return RepositoryError{"Submission is not retryable or was already retried"};
  }
  auto retried = read_record(statement.get());
  if (const auto *error = std::get_if<RepositoryError>(&retried)) {
    rollback();
    return *error;
  }
  sqlite3_reset(statement.get());
  const auto &record = std::get<SubmissionRecord>(retried);
  Statement audit_statement{
      implementation_->database,
      "INSERT INTO admin_audit_log(admin_user_id, action, entity_type, entity_id, details_json, "
      "created_at) VALUES (?, 'submission.retry', 'submission', ?, ?, "
      "strftime('%Y-%m-%dT%H:%M:%fZ', 'now'));"};
  if (!audit_statement.valid()) {
    rollback();
    return RepositoryError{audit_statement.error()};
  }
  sqlite3_bind_int64(audit_statement.get(), 1, admin_user_id);
  bind_text(audit_statement.get(), 2, std::to_string(submission_id.value));
  bind_text(audit_statement.get(), 3,
            "{\"retrySubmissionId\":" + std::to_string(record.id.value) + "}");
  if (sqlite3_step(audit_statement.get()) != SQLITE_DONE) {
    rollback();
    return database_error(implementation_->database, "Could not audit submission retry");
  }
  if (const auto error = execute(implementation_->database, "COMMIT;")) {
    rollback();
    return *error;
  }
  return retried;
}

std::variant<std::monostate, RepositoryError>
SQLiteSubmissionRepository::audit(std::int64_t admin_user_id, const std::string &action,
                                  const std::string &entity_type, const std::string &entity_id,
                                  const std::string &details_json) {
  Statement statement{
      implementation_->database,
      "INSERT INTO admin_audit_log(admin_user_id, action, entity_type, entity_id, details_json, "
      "created_at) VALUES (?, ?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'));"};
  if (!statement.valid())
    return RepositoryError{statement.error()};
  sqlite3_bind_int64(statement.get(), 1, admin_user_id);
  bind_text(statement.get(), 2, action);
  bind_text(statement.get(), 3, entity_type);
  bind_text(statement.get(), 4, entity_id);
  bind_text(statement.get(), 5, details_json);
  if (sqlite3_step(statement.get()) != SQLITE_DONE)
    return database_error(implementation_->database, "Could not write admin audit record");
  return std::monostate{};
}

UserProgressResult SQLiteSubmissionRepository::progress(std::int64_t user_id) {
  Statement totals{implementation_->database,
                   "SELECT COUNT(*), COUNT(CASE WHEN verdict = 'Accepted' THEN 1 END) "
                   "FROM submissions WHERE user_id = ?;"};
  if (!totals.valid()) {
    return RepositoryError{totals.error()};
  }
  sqlite3_bind_int64(totals.get(), 1, user_id);
  if (sqlite3_step(totals.get()) != SQLITE_ROW) {
    return database_error(implementation_->database, "Could not retrieve user activity");
  }

  UserProgress progress{
      .total_submissions = sqlite3_column_int64(totals.get(), 0),
      .accepted_submissions = sqlite3_column_int64(totals.get(), 1),
  };
  Statement most_recent{implementation_->database,
                        "SELECT problem_id FROM submissions WHERE user_id = ? "
                        "ORDER BY created_at DESC, id DESC LIMIT 1;"};
  if (!most_recent.valid()) {
    return RepositoryError{most_recent.error()};
  }
  sqlite3_bind_int64(most_recent.get(), 1, user_id);
  const auto most_recent_result = sqlite3_step(most_recent.get());
  if (most_recent_result == SQLITE_ROW) {
    progress.most_recent_submission_problem_id = column_text(most_recent.get(), 0);
  } else if (most_recent_result != SQLITE_DONE) {
    return database_error(implementation_->database,
                          "Could not retrieve most recent submission problem");
  }

  Statement streak{
      implementation_->database,
      "WITH days AS ("
      " SELECT DISTINCT date(completed_at) AS day FROM submissions"
      " WHERE user_id = ? AND verdict = 'Accepted' AND completed_at IS NOT NULL"
      "), ordered AS ("
      " SELECT day, CAST(julianday((SELECT MAX(day) FROM days)) - julianday(day) AS INTEGER)"
      " AS distance, ROW_NUMBER() OVER (ORDER BY day DESC) - 1 AS position FROM days"
      ") SELECT CASE WHEN COALESCE((SELECT MAX(day) FROM days), '') < date('now', '-1 day')"
      " THEN 0 ELSE COUNT(CASE WHEN distance = position THEN 1 END) END FROM ordered;"};
  if (!streak.valid()) {
    return RepositoryError{streak.error()};
  }
  sqlite3_bind_int64(streak.get(), 1, user_id);
  if (sqlite3_step(streak.get()) != SQLITE_ROW) {
    return database_error(implementation_->database, "Could not retrieve user streak");
  }
  progress.current_streak_days = sqlite3_column_int64(streak.get(), 0);

  Statement completed{implementation_->database,
                      "SELECT problem_id, MIN(completed_at) FROM submissions "
                      "WHERE user_id = ? AND verdict = 'Accepted' AND completed_at IS NOT NULL "
                      "GROUP BY problem_id ORDER BY MIN(completed_at) DESC;"};
  if (!completed.valid()) {
    return RepositoryError{completed.error()};
  }
  sqlite3_bind_int64(completed.get(), 1, user_id);
  while (true) {
    const auto result = sqlite3_step(completed.get());
    if (result == SQLITE_DONE) {
      return progress;
    }
    if (result != SQLITE_ROW) {
      return database_error(implementation_->database, "Could not retrieve completed problems");
    }
    progress.completed_problems.push_back({
        .problem_id = column_text(completed.get(), 0),
        .completed_at = column_text(completed.get(), 1),
    });
  }
}

} // namespace algorithm_trainer
