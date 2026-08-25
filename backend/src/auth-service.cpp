#include "algorithm-trainer/auth-service.h"

#include <sodium.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace algorithm_trainer {
namespace {

constexpr int migration_version{2};
constexpr int busy_timeout_milliseconds{5000};
constexpr int session_lifetime_seconds{60 * 60 * 24 * 30};

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

AuthError internal_error(std::string message) {
  return {AuthErrorCode::internal, std::move(message)};
}

void bind_text(sqlite3_stmt *statement, int parameter, const std::string &value) {
  sqlite3_bind_text(statement, parameter, value.data(), static_cast<int>(value.size()),
                    SQLITE_TRANSIENT);
}

std::string column_text(sqlite3_stmt *statement, int column) {
  const auto *text = sqlite3_column_text(statement, column);
  const auto size = sqlite3_column_bytes(statement, column);
  return text == nullptr
             ? std::string{}
             : std::string{reinterpret_cast<const char *>(text), static_cast<std::size_t>(size)};
}

std::optional<AuthError> execute(sqlite3 *database, std::string_view sql) {
  char *message{};
  if (sqlite3_exec(database, std::string{sql}.c_str(), nullptr, nullptr, &message) == SQLITE_OK) {
    return std::nullopt;
  }
  std::string detail = message == nullptr ? sqlite3_errmsg(database) : message;
  sqlite3_free(message);
  return internal_error("SQLite operation failed: " + detail);
}

std::optional<AuthError> apply_migration(sqlite3 *database) {
  if (const auto error =
          execute(database,
                  "CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY);")) {
    return error;
  }
  Statement check{database, "SELECT 1 FROM schema_migrations WHERE version = ?;"};
  if (!check.valid()) {
    return internal_error(check.error());
  }
  sqlite3_bind_int(check.get(), 1, migration_version);
  if (sqlite3_step(check.get()) == SQLITE_ROW) {
    return std::nullopt;
  }

  const auto path = std::filesystem::path{ALGORITHM_TRAINER_MIGRATIONS_DIR} / "002-create-auth.sql";
  std::ifstream input{path};
  if (!input) {
    return internal_error("Authentication migration file is unavailable");
  }
  const std::string migration{std::istreambuf_iterator<char>{input},
                              std::istreambuf_iterator<char>{}};
  if (const auto error = execute(database, "BEGIN IMMEDIATE;")) {
    return error;
  }
  if (const auto error = execute(database, migration)) {
    static_cast<void>(execute(database, "ROLLBACK;"));
    return error;
  }
  Statement record{database, "INSERT INTO schema_migrations(version) VALUES (?);"};
  sqlite3_bind_int(record.get(), 1, migration_version);
  if (!record.valid() || sqlite3_step(record.get()) != SQLITE_DONE) {
    auto error = internal_error("Could not record authentication migration");
    static_cast<void>(execute(database, "ROLLBACK;"));
    return error;
  }
  return execute(database, "COMMIT;");
}

std::optional<AuthError> validate_credentials(const std::string &username,
                                              const std::string &password) {
  if (username.size() < 3 || username.size() > 64) {
    return AuthError{AuthErrorCode::invalid_input,
                     "Username must contain between 3 and 64 characters"};
  }
  const auto valid_username_character = [](unsigned char character) {
    return std::isalnum(character) != 0 || character == '_' || character == '-' || character == '.';
  };
  if (!std::ranges::all_of(username, valid_username_character)) {
    return AuthError{AuthErrorCode::invalid_input,
                     "Username may contain only letters, numbers, dots, underscores, and hyphens"};
  }
  if (password.size() < 8 || password.size() > 128) {
    return AuthError{AuthErrorCode::invalid_input,
                     "Password must contain between 8 and 128 characters"};
  }
  return std::nullopt;
}

std::string token_hash(const std::string &token) {
  std::array<unsigned char, crypto_generichash_BYTES> digest{};
  crypto_generichash(digest.data(), digest.size(),
                     reinterpret_cast<const unsigned char *>(token.data()), token.size(), nullptr,
                     0);
  std::array<char, crypto_generichash_BYTES * 2 + 1> encoded{};
  sodium_bin2hex(encoded.data(), encoded.size(), digest.data(), digest.size());
  return encoded.data();
}

std::string new_token() {
  std::array<unsigned char, 32> bytes{};
  randombytes_buf(bytes.data(), bytes.size());
  std::array<char,
             sodium_base64_ENCODED_LEN(bytes.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING)>
      encoded{};
  sodium_bin2base64(encoded.data(), encoded.size(), bytes.data(), bytes.size(),
                    sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  return encoded.data();
}

AuthUser read_user(sqlite3_stmt *statement) {
  return {.id = sqlite3_column_int64(statement, 0),
          .username = column_text(statement, 1),
          .created_at = column_text(statement, 2)};
}

std::variant<AuthSession, AuthError> create_session(sqlite3 *database, AuthUser user) {
  auto token = new_token();
  const auto hashed_token = token_hash(token);
  Statement statement{database, "INSERT INTO sessions(user_id, token_hash, created_at, expires_at) "
                                "VALUES (?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
                                "strftime('%Y-%m-%dT%H:%M:%fZ', 'now', '+' || ? || ' seconds'));"};
  if (!statement.valid()) {
    return internal_error(statement.error());
  }
  sqlite3_bind_int64(statement.get(), 1, user.id);
  bind_text(statement.get(), 2, hashed_token);
  sqlite3_bind_int(statement.get(), 3, session_lifetime_seconds);
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    return internal_error("Could not create session");
  }
  return AuthSession{.user = std::move(user), .token = std::move(token)};
}

} // namespace

struct AuthService::Implementation {
  sqlite3 *database{};
  std::string dummy_password_hash;
  ~Implementation() { sqlite3_close(database); }
};

AuthService::AuthService(std::unique_ptr<Implementation> implementation)
    : implementation_{std::move(implementation)} {}
AuthService::~AuthService() = default;

AuthService::OpenResult AuthService::open(const std::filesystem::path &database_path) {
  if (sodium_init() < 0) {
    return internal_error("Cryptographic library initialization failed");
  }
  if (database_path != ":memory:" && database_path.has_parent_path()) {
    std::error_code error;
    std::filesystem::create_directories(database_path.parent_path(), error);
    if (error) {
      return internal_error("Could not create database directory: " + error.message());
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
    return internal_error("Could not open authentication database: " + message);
  }
  auto implementation = std::make_unique<Implementation>();
  implementation->database = database;
  std::array<char, crypto_pwhash_STRBYTES> dummy_hash{};
  constexpr std::string_view dummy_password{"authentication-timing-placeholder"};
  if (crypto_pwhash_str(dummy_hash.data(), dummy_password.data(), dummy_password.size(),
                        crypto_pwhash_OPSLIMIT_INTERACTIVE,
                        crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
    return internal_error("Password hashing initialization failed");
  }
  implementation->dummy_password_hash = dummy_hash.data();
  sqlite3_busy_timeout(database, busy_timeout_milliseconds);
  if (const auto error = execute(database, "PRAGMA foreign_keys = ON;")) {
    return *error;
  }
  if (const auto error = apply_migration(database)) {
    return *error;
  }
  return std::unique_ptr<AuthService>{new AuthService{std::move(implementation)}};
}

AuthService::SessionResult AuthService::register_user(std::string username, std::string password) {
  if (const auto error = validate_credentials(username, password)) {
    return *error;
  }
  std::array<char, crypto_pwhash_STRBYTES> password_hash{};
  if (crypto_pwhash_str(password_hash.data(), password.data(), password.size(),
                        crypto_pwhash_OPSLIMIT_INTERACTIVE,
                        crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
    return internal_error("Password hashing failed");
  }
  Statement statement{
      implementation_->database,
      "INSERT INTO users(username, password_hash, created_at) "
      "VALUES (?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now')) RETURNING id, username, created_at;"};
  if (!statement.valid()) {
    return internal_error(statement.error());
  }
  bind_text(statement.get(), 1, username);
  const std::string stored_hash{password_hash.data()};
  bind_text(statement.get(), 2, stored_hash);
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    if (sqlite3_extended_errcode(implementation_->database) == SQLITE_CONSTRAINT_UNIQUE) {
      return AuthError{AuthErrorCode::duplicate_user, "Username is already registered"};
    }
    return internal_error("Could not create user");
  }
  return create_session(implementation_->database, read_user(statement.get()));
}

AuthService::SessionResult AuthService::login(std::string username, std::string password) {
  if (username.empty() || username.size() > 64 || password.empty() || password.size() > 128) {
    return AuthError{AuthErrorCode::invalid_credentials, "Invalid username or password"};
  }
  Statement statement{implementation_->database,
                      "SELECT id, username, created_at, password_hash FROM users "
                      "WHERE username = ? COLLATE NOCASE;"};
  if (!statement.valid()) {
    return internal_error(statement.error());
  }
  bind_text(statement.get(), 1, username);
  const bool user_exists = sqlite3_step(statement.get()) == SQLITE_ROW;
  const auto password_hash =
      user_exists ? column_text(statement.get(), 3) : implementation_->dummy_password_hash;
  const bool password_matches =
      crypto_pwhash_str_verify(password_hash.c_str(), password.data(), password.size()) == 0;
  if (!user_exists || !password_matches) {
    return AuthError{AuthErrorCode::invalid_credentials, "Invalid username or password"};
  }
  return create_session(implementation_->database, read_user(statement.get()));
}

AuthService::UserResult AuthService::current_user(const std::string &token) {
  if (token.empty()) {
    return AuthError{AuthErrorCode::unauthenticated, "Authentication required"};
  }
  Statement statement{implementation_->database,
                      "SELECT users.id, users.username, users.created_at FROM sessions "
                      "JOIN users ON users.id = sessions.user_id "
                      "WHERE sessions.token_hash = ? AND sessions.expires_at > "
                      "strftime('%Y-%m-%dT%H:%M:%fZ', 'now');"};
  if (!statement.valid()) {
    return internal_error(statement.error());
  }
  bind_text(statement.get(), 1, token_hash(token));
  if (sqlite3_step(statement.get()) != SQLITE_ROW) {
    return AuthError{AuthErrorCode::unauthenticated, "Authentication required"};
  }
  return read_user(statement.get());
}

std::variant<std::monostate, AuthError> AuthService::logout(const std::string &token) {
  if (token.empty()) {
    return std::monostate{};
  }
  Statement statement{implementation_->database, "DELETE FROM sessions WHERE token_hash = ?;"};
  if (!statement.valid()) {
    return internal_error(statement.error());
  }
  bind_text(statement.get(), 1, token_hash(token));
  if (sqlite3_step(statement.get()) != SQLITE_DONE) {
    return internal_error("Could not invalidate session");
  }
  return std::monostate{};
}

} // namespace algorithm_trainer
