#pragma once

#include <libpq-fe.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace algorithm_trainer::postgresql {

class Result {
public:
  explicit Result(PGresult *result = nullptr) : result_{result, &PQclear} {}
  [[nodiscard]] bool command_ok() const;
  [[nodiscard]] bool tuples_ok() const;
  [[nodiscard]] int rows() const;
  [[nodiscard]] std::string value(int row, int column) const;
  [[nodiscard]] bool is_null(int row, int column) const;
  [[nodiscard]] std::string error() const;

private:
  std::unique_ptr<PGresult, decltype(&PQclear)> result_{nullptr, &PQclear};
};

class Connection {
public:
  explicit Connection(const std::string &connection_info);
  ~Connection();
  Connection(const Connection &) = delete;
  [[nodiscard]] bool valid() const;
  [[nodiscard]] std::string error() const;
  [[nodiscard]] Result execute(std::string_view sql,
                               const std::vector<std::optional<std::string>> &parameters = {});
  [[nodiscard]] Result execute_locked(
      std::int64_t advisory_lock, std::string_view sql,
      const std::vector<std::optional<std::string>> &parameters = {});

private:
  PGconn *connection_{};
  mutable std::mutex mutex_;
};

[[nodiscard]] std::optional<std::string> apply_migrations(const std::string &connection_info);

[[nodiscard]] inline std::optional<std::string> text(std::optional<std::string> value) {
  return value;
}
[[nodiscard]] inline std::optional<std::string> text(const std::string &value) { return value; }
[[nodiscard]] inline std::optional<std::string> text(std::string_view value) {
  return std::string{value};
}
[[nodiscard]] inline std::optional<std::string> number(std::int64_t value) {
  return std::to_string(value);
}
[[nodiscard]] inline std::optional<std::string> boolean(bool value) {
  return value ? "true" : "false";
}

} // namespace algorithm_trainer::postgresql
