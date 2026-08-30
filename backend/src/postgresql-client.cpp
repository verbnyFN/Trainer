#include "postgresql-client.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace algorithm_trainer::postgresql {

bool Result::command_ok() const {
  return result_ && PQresultStatus(result_.get()) == PGRES_COMMAND_OK;
}
bool Result::tuples_ok() const {
  return result_ && PQresultStatus(result_.get()) == PGRES_TUPLES_OK;
}
int Result::rows() const { return result_ ? PQntuples(result_.get()) : 0; }
std::string Result::value(int row, int column) const {
  if (!result_ || PQgetisnull(result_.get(), row, column))
    return {};
  return {PQgetvalue(result_.get(), row, column),
          static_cast<std::size_t>(PQgetlength(result_.get(), row, column))};
}
bool Result::is_null(int row, int column) const {
  return !result_ || PQgetisnull(result_.get(), row, column) != 0;
}
std::string Result::error() const {
  return result_ ? PQresultErrorMessage(result_.get()) : "No result";
}

Connection::Connection(const std::string &connection_info)
    : connection_{PQconnectdb(connection_info.c_str())} {}
Connection::~Connection() { PQfinish(connection_); }
bool Connection::valid() const { return connection_ && PQstatus(connection_) == CONNECTION_OK; }
std::string Connection::error() const {
  return connection_ ? PQerrorMessage(connection_) : "No connection";
}
Result Connection::execute(std::string_view sql,
                           const std::vector<std::optional<std::string>> &parameters) {
  std::lock_guard lock{mutex_};
  if (parameters.empty())
    return Result{PQexec(connection_, std::string{sql}.c_str())};
  std::vector<const char *> values;
  values.reserve(parameters.size());
  for (const auto &parameter : parameters)
    values.push_back(parameter ? parameter->c_str() : nullptr);
  return Result{PQexecParams(connection_, std::string{sql}.c_str(), static_cast<int>(values.size()),
                             nullptr, values.data(), nullptr, nullptr, 0)};
}

Result Connection::execute_locked(std::int64_t advisory_lock, std::string_view sql,
                                  const std::vector<std::optional<std::string>> &parameters) {
  std::lock_guard lock{mutex_};
  auto command = [this](const char *value) { return Result{PQexec(connection_, value)}; };
  if (!command("BEGIN").command_ok())
    return Result{};
  const auto lock_sql = "SELECT pg_advisory_xact_lock(" + std::to_string(advisory_lock) + ")";
  if (!command(lock_sql.c_str()).tuples_ok()) {
    static_cast<void>(command("ROLLBACK"));
    return Result{};
  }
  std::vector<const char *> values;
  values.reserve(parameters.size());
  for (const auto &parameter : parameters)
    values.push_back(parameter ? parameter->c_str() : nullptr);
  Result result{parameters.empty() ? PQexec(connection_, std::string{sql}.c_str())
                                   : PQexecParams(connection_, std::string{sql}.c_str(),
                                                  static_cast<int>(values.size()), nullptr,
                                                  values.data(), nullptr, nullptr, 0)};
  if (!result.command_ok() && !result.tuples_ok()) {
    static_cast<void>(command("ROLLBACK"));
    return result;
  }
  if (!command("COMMIT").command_ok()) {
    static_cast<void>(command("ROLLBACK"));
    return Result{};
  }
  return result;
}

std::optional<std::string> apply_migrations(const std::string &connection_info) {
  Connection connection{connection_info};
  if (!connection.valid())
    return "Could not connect to PostgreSQL: " + connection.error();
  if (!connection.execute("SELECT pg_advisory_lock(784512901)").tuples_ok())
    return "Could not acquire the PostgreSQL migration lock";
  auto versions = connection.execute(
      "CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, applied_at "
      "TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp())");
  if (!versions.command_ok())
    return "Could not initialize PostgreSQL migration tracking: " + versions.error();
  const auto directory = std::filesystem::path{ALGORITHM_TRAINER_MIGRATIONS_DIR} / "postgresql";
  std::vector<std::filesystem::path> migrations;
  std::error_code filesystem_error;
  for (std::filesystem::directory_iterator entry{directory, filesystem_error};
       !filesystem_error && entry != std::filesystem::directory_iterator{};
       entry.increment(filesystem_error)) {
    if (entry->is_regular_file() && entry->path().extension() == ".sql")
      migrations.push_back(entry->path());
  }
  if (filesystem_error) {
    return "Could not enumerate PostgreSQL migrations: " + filesystem_error.message();
  }
  std::ranges::sort(migrations);
  for (const auto &path : migrations) {
    const auto name = path.filename().string();
    const auto separator = name.find('-');
    int version{};
    if (separator == std::string::npos ||
        std::from_chars(name.data(), name.data() + separator, version).ec != std::errc{} ||
        version <= 0) {
      return "PostgreSQL migration has an invalid numbered filename: " + name;
    }
    auto applied =
        connection.execute("SELECT 1 FROM schema_migrations WHERE version=$1", {number(version)});
    if (!applied.tuples_ok())
      return "Could not inspect PostgreSQL migration state: " + applied.error();
    if (applied.rows() != 0)
      continue;
    std::ifstream input{path};
    if (!input)
      return "PostgreSQL migration file is unavailable: " + name;
    const std::string sql{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    if (!connection.execute("BEGIN").command_ok())
      return "Could not begin PostgreSQL migration: " + name;
    auto migrated = connection.execute(sql);
    auto recorded = migrated.command_ok()
                        ? connection.execute("INSERT INTO schema_migrations(version) VALUES($1)",
                                             {number(version)})
                        : Result{};
    if (!migrated.command_ok() || !recorded.command_ok() ||
        !connection.execute("COMMIT").command_ok()) {
      static_cast<void>(connection.execute("ROLLBACK"));
      return "PostgreSQL migration failed: " + name + ": " + migrated.error();
    }
  }
  static_cast<void>(connection.execute("SELECT pg_advisory_unlock(784512901)"));
  return std::nullopt;
}

} // namespace algorithm_trainer::postgresql
