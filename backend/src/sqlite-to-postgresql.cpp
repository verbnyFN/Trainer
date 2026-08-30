#include "postgresql-client.h"

#include <sqlite3.h>

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
struct Table {
  std::string name;
  std::vector<std::string> columns;
  std::vector<std::string> expressions;
};

std::string join(const std::vector<std::string> &values, std::string_view separator) {
  std::string result;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0)
      result += separator;
    result += values[index];
  }
  return result;
}

bool transfer(sqlite3 *source, algorithm_trainer::postgresql::Connection &target,
              const Table &table, std::int64_t &copied) {
  const auto select = "SELECT " + join(table.columns, ",") + " FROM " + table.name + " ORDER BY id";
  sqlite3_stmt *statement{};
  if (sqlite3_prepare_v2(source, select.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
    std::cerr << "Could not read " << table.name << ": " << sqlite3_errmsg(source) << '\n';
    return false;
  }
  std::vector<std::string> placeholders;
  for (std::size_t index = 0; index < table.columns.size(); ++index) {
    auto placeholder = "$" + std::to_string(index + 1);
    if (index < table.expressions.size() && !table.expressions[index].empty())
      placeholder += table.expressions[index];
    placeholders.push_back(std::move(placeholder));
  }
  const auto insert = "INSERT INTO " + table.name + "(" + join(table.columns, ",") + ") VALUES(" +
                      join(placeholders, ",") + ")";
  while (sqlite3_step(statement) == SQLITE_ROW) {
    std::vector<std::optional<std::string>> parameters;
    for (int column = 0; column < sqlite3_column_count(statement); ++column) {
      if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        parameters.emplace_back(std::nullopt);
      } else {
        const auto *value = sqlite3_column_text(statement, column);
        parameters.emplace_back(
            std::string{reinterpret_cast<const char *>(value),
                        static_cast<std::size_t>(sqlite3_column_bytes(statement, column))});
      }
    }
    auto result = target.execute(insert, parameters);
    if (!result.command_ok()) {
      std::cerr << "Could not insert " << table.name << ": " << result.error() << '\n';
      sqlite3_finalize(statement);
      return false;
    }
    ++copied;
  }
  const auto status = sqlite3_errcode(source);
  sqlite3_finalize(statement);
  if (status != SQLITE_OK && status != SQLITE_DONE) {
    std::cerr << "Could not finish reading " << table.name << ": " << sqlite3_errmsg(source)
              << '\n';
    return false;
  }
  return true;
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "Usage: algorithm-trainer-migrate-sqlite <sqlite-file> <postgresql-url>\n";
    return 2;
  }
  sqlite3 *source{};
  if (sqlite3_open_v2(argv[1], &source, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    std::cerr << "Could not open the SQLite source database\n";
    sqlite3_close(source);
    return 1;
  }
  algorithm_trainer::postgresql::Connection target{argv[2]};
  if (!target.valid()) {
    std::cerr << "Could not connect to PostgreSQL: " << target.error() << '\n';
    sqlite3_close(source);
    return 1;
  }
  if (sqlite3_exec(source, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK ||
      !target.execute("BEGIN").command_ok() ||
      !target.execute("SELECT pg_advisory_xact_lock(784512905)").tuples_ok() ||
      !target
           .execute("LOCK TABLE admin_audit_log, sessions, submissions, problem_tests, problems, "
                    "users IN ACCESS EXCLUSIVE MODE")
           .command_ok()) {
    std::cerr << "Could not establish transfer snapshots and locks\n";
    static_cast<void>(target.execute("ROLLBACK"));
    sqlite3_close(source);
    return 1;
  }
  auto nonempty =
      target.execute("SELECT (SELECT count(*) FROM users)+(SELECT count(*) FROM problems)+"
                     "(SELECT count(*) FROM submissions)+(SELECT count(*) FROM sessions)+"
                     "(SELECT count(*) FROM admin_audit_log)");
  if (!nonempty.tuples_ok() || nonempty.value(0, 0) != "0") {
    std::cerr << "Refusing transfer: PostgreSQL target is not empty\n";
    static_cast<void>(target.execute("ROLLBACK"));
    sqlite3_close(source);
    return 1;
  }
  const std::vector<Table> tables{
      {"users",
       {"id", "username", "password_hash", "created_at", "is_admin"},
       {"::bigint", "", "", "::timestamptz", "::integer::boolean"}},
      {"sessions",
       {"id", "user_id", "token_hash", "created_at", "expires_at"},
       {"::bigint", "::bigint", "", "::timestamptz", "::timestamptz"}},
      {"problems",
       {"id", "title", "description", "input_format", "output_format", "difficulty", "tags_json",
        "languages_json", "examples_json", "enabled", "created_at", "updated_at", "revision"},
       {"", "", "", "", "", "", "::jsonb", "::jsonb", "::jsonb", "::integer::boolean",
        "::timestamptz", "::timestamptz", "::bigint"}},
      {"problem_tests",
       {"id", "problem_id", "input", "expected_output", "position", "enabled", "revision"},
       {"::bigint", "", "", "", "::bigint", "::integer::boolean", "::bigint"}},
      {"submissions",
       {"id", "problem_id", "language", "source_code", "status", "verdict", "created_at",
        "completed_at", "user_id", "error_type", "retry_of"},
       {"::bigint", "", "", "", "", "", "::timestamptz", "::timestamptz", "::bigint", "",
        "::bigint"}},
      {"admin_audit_log",
       {"id", "admin_user_id", "action", "entity_type", "entity_id", "details_json", "created_at"},
       {"::bigint", "::bigint", "", "", "", "::jsonb", "::timestamptz"}},
  };
  std::int64_t total{};
  for (const auto &table : tables) {
    std::int64_t copied{};
    if (!transfer(source, target, table, copied)) {
      static_cast<void>(target.execute("ROLLBACK"));
      sqlite3_close(source);
      return 1;
    }
    total += copied;
    std::cout << table.name << ": " << copied << " rows\n";
  }
  auto sequences = target.execute(
      "SELECT setval(pg_get_serial_sequence('users','id'),coalesce(max(id),1),max(id) IS NOT NULL) "
      "FROM users;"
      "SELECT setval(pg_get_serial_sequence('sessions','id'),coalesce(max(id),1),max(id) IS NOT "
      "NULL) FROM sessions;"
      "SELECT setval(pg_get_serial_sequence('problem_tests','id'),coalesce(max(id),1),max(id) IS "
      "NOT NULL) FROM problem_tests;"
      "SELECT setval(pg_get_serial_sequence('submissions','id'),coalesce(max(id),1),max(id) IS NOT "
      "NULL) FROM submissions;"
      "SELECT setval(pg_get_serial_sequence('admin_audit_log','id'),coalesce(max(id),1),max(id) IS "
      "NOT NULL) FROM admin_audit_log;");
  if (!sequences.tuples_ok() || !target.execute("COMMIT").command_ok()) {
    static_cast<void>(target.execute("ROLLBACK"));
    std::cerr << "Could not finalize PostgreSQL transfer\n";
    sqlite3_close(source);
    return 1;
  }
  sqlite3_close(source);
  std::cout << "Transfer completed atomically: " << total << " rows\n";
  return 0;
}
