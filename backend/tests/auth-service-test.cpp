#include "algorithm-trainer/auth-service.h"

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include <array>
#include <filesystem>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace {

class TemporaryDatabase {
public:
  TemporaryDatabase() {
    std::array<char, 64> path_template{};
    constexpr std::string_view pattern{"/tmp/algorithm-trainer-auth-XXXXXX"};
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

std::unique_ptr<algorithm_trainer::AuthService> open_service(const std::filesystem::path &path) {
  auto result = algorithm_trainer::AuthService::open(path);
  REQUIRE(std::holds_alternative<std::unique_ptr<algorithm_trainer::AuthService>>(result));
  return std::get<std::unique_ptr<algorithm_trainer::AuthService>>(std::move(result));
}

algorithm_trainer::AuthSession session(algorithm_trainer::AuthService::SessionResult result) {
  REQUIRE(std::holds_alternative<algorithm_trainer::AuthSession>(result));
  return std::get<algorithm_trainer::AuthSession>(std::move(result));
}

algorithm_trainer::AuthError error(algorithm_trainer::AuthService::SessionResult result) {
  REQUIRE(std::holds_alternative<algorithm_trainer::AuthError>(result));
  return std::get<algorithm_trainer::AuthError>(std::move(result));
}

} // namespace

TEST_CASE("Registration creates a user and authenticated session", "[auth]") {
  TemporaryDatabase database;
  auto service = open_service(database.path());

  const auto registered = session(service->register_user("alice", "correct horse battery"));
  const auto current = service->current_user(registered.token);

  CHECK(registered.user.id > 0);
  CHECK(registered.user.username == "alice");
  REQUIRE(std::holds_alternative<algorithm_trainer::AuthUser>(current));
  CHECK(std::get<algorithm_trainer::AuthUser>(current).username == "alice");
}

TEST_CASE("Duplicate registration is rejected case-insensitively", "[auth]") {
  TemporaryDatabase database;
  auto service = open_service(database.path());
  static_cast<void>(session(service->register_user("alice", "password-one")));

  const auto duplicate = error(service->register_user("ALICE", "password-two"));

  CHECK(duplicate.code == algorithm_trainer::AuthErrorCode::duplicate_user);
}

TEST_CASE("Login accepts valid credentials and rejects invalid credentials", "[auth]") {
  TemporaryDatabase database;
  auto service = open_service(database.path());
  static_cast<void>(session(service->register_user("alice", "password-one")));

  CHECK(std::holds_alternative<algorithm_trainer::AuthSession>(
      service->login("alice", "password-one")));
  CHECK(error(service->login("alice", "incorrect-password")).code ==
        algorithm_trainer::AuthErrorCode::invalid_credentials);
  CHECK(error(service->login("unknown", "password-one")).code ==
        algorithm_trainer::AuthErrorCode::invalid_credentials);
}

TEST_CASE("Missing session is unauthenticated", "[auth]") {
  TemporaryDatabase database;
  auto service = open_service(database.path());

  const auto result = service->current_user("");

  REQUIRE(std::holds_alternative<algorithm_trainer::AuthError>(result));
  CHECK(std::get<algorithm_trainer::AuthError>(result).code ==
        algorithm_trainer::AuthErrorCode::unauthenticated);
}

TEST_CASE("Logout invalidates the server-side session", "[auth]") {
  TemporaryDatabase database;
  auto service = open_service(database.path());
  const auto registered = session(service->register_user("alice", "password-one"));

  CHECK(std::holds_alternative<std::monostate>(service->logout(registered.token)));
  const auto current = service->current_user(registered.token);

  REQUIRE(std::holds_alternative<algorithm_trainer::AuthError>(current));
  CHECK(std::get<algorithm_trainer::AuthError>(current).code ==
        algorithm_trainer::AuthErrorCode::unauthenticated);
}

TEST_CASE("Passwords and raw session tokens are not stored", "[auth]") {
  TemporaryDatabase database;
  const std::string password{"password-one"};
  std::string token;
  {
    auto service = open_service(database.path());
    token = session(service->register_user("alice", password)).token;
  }

  sqlite3 *connection{};
  REQUIRE(sqlite3_open_v2(database.path().c_str(), &connection, SQLITE_OPEN_READONLY, nullptr) ==
          SQLITE_OK);
  sqlite3_stmt *statement{};
  REQUIRE(sqlite3_prepare_v2(connection,
                             "SELECT users.password_hash, sessions.token_hash FROM users "
                             "JOIN sessions ON sessions.user_id = users.id;",
                             -1, &statement, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(statement) == SQLITE_ROW);
  const std::string password_hash{
      reinterpret_cast<const char *>(sqlite3_column_text(statement, 0))};
  const std::string stored_token_hash{
      reinterpret_cast<const char *>(sqlite3_column_text(statement, 1))};

  CHECK(password_hash != password);
  CHECK(password_hash.starts_with("$argon2id$"));
  CHECK(stored_token_hash != token);
  CHECK(stored_token_hash.size() == 64);
  sqlite3_finalize(statement);
  sqlite3_close(connection);
}

TEST_CASE("Registration validates username and password", "[auth]") {
  TemporaryDatabase database;
  auto service = open_service(database.path());

  CHECK(error(service->register_user("", "password-one")).code ==
        algorithm_trainer::AuthErrorCode::invalid_input);
  CHECK(error(service->register_user("bad name", "password-one")).code ==
        algorithm_trainer::AuthErrorCode::invalid_input);
  CHECK(error(service->register_user("alice", "short")).code ==
        algorithm_trainer::AuthErrorCode::invalid_input);
  CHECK(error(service->register_user(std::string(65, 'a'), "password-one")).code ==
        algorithm_trainer::AuthErrorCode::invalid_input);
  CHECK(error(service->register_user("alice", std::string(129, 'p'))).code ==
        algorithm_trainer::AuthErrorCode::invalid_input);
}
