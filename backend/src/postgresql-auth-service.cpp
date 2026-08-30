#include "algorithm-trainer/auth-service.h"
#include "postgresql-client.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <ranges>

namespace algorithm_trainer {
namespace {
constexpr int session_lifetime_seconds{60 * 60 * 24 * 30};
AuthError pg_internal(std::string message) { return {AuthErrorCode::internal, std::move(message)}; }
std::optional<AuthError> pg_validate(const std::string &username, const std::string &password) {
  if (username.size() < 3 || username.size() > 64)
    return AuthError{AuthErrorCode::invalid_input,
                     "Username must contain between 3 and 64 characters"};
  if (!std::ranges::all_of(username, [](unsigned char value) {
        return std::isalnum(value) != 0 || value == '_' || value == '-' || value == '.';
      }))
    return AuthError{AuthErrorCode::invalid_input,
                     "Username may contain only letters, numbers, dots, underscores, and hyphens"};
  if (password.size() < 8 || password.size() > 128)
    return AuthError{AuthErrorCode::invalid_input,
                     "Password must contain between 8 and 128 characters"};
  return std::nullopt;
}
std::string pg_token_hash(const std::string &token) {
  std::array<unsigned char, crypto_generichash_BYTES> digest{};
  crypto_generichash(digest.data(), digest.size(),
                     reinterpret_cast<const unsigned char *>(token.data()), token.size(), nullptr,
                     0);
  std::array<char, crypto_generichash_BYTES * 2 + 1> encoded{};
  sodium_bin2hex(encoded.data(), encoded.size(), digest.data(), digest.size());
  return encoded.data();
}
std::string pg_new_token() {
  std::array<unsigned char, 32> bytes{};
  randombytes_buf(bytes.data(), bytes.size());
  std::array<char,
             sodium_base64_ENCODED_LEN(bytes.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING)>
      encoded{};
  sodium_bin2base64(encoded.data(), encoded.size(), bytes.data(), bytes.size(),
                    sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  return encoded.data();
}
std::int64_t pg_integer(const std::string &value) {
  std::int64_t result{};
  std::from_chars(value.data(), value.data() + value.size(), result);
  return result;
}
AuthUser pg_user(const postgresql::Result &result, int row = 0) {
  return {.id = pg_integer(result.value(row, 0)),
          .username = result.value(row, 1),
          .created_at = result.value(row, 2),
          .is_admin = result.value(row, 3) == "t"};
}
std::string password_hash(const std::string &password) {
  std::array<char, crypto_pwhash_STRBYTES> hash{};
  if (crypto_pwhash_str(hash.data(), password.data(), password.size(),
                        crypto_pwhash_OPSLIMIT_INTERACTIVE,
                        crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
    return {};
  return hash.data();
}
} // namespace

struct PostgreSQLAuthService::Implementation {
  postgresql::Connection database;
  std::string dummy_password_hash;
  explicit Implementation(const std::string &info) : database{info} {}
};
PostgreSQLAuthService::PostgreSQLAuthService(std::unique_ptr<Implementation> value)
    : implementation_{std::move(value)} {}
PostgreSQLAuthService::~PostgreSQLAuthService() = default;
PostgreSQLAuthService::OpenResult PostgreSQLAuthService::open(const std::string &info) {
  if (sodium_init() < 0)
    return pg_internal("Cryptographic library initialization failed");
  auto impl = std::make_unique<Implementation>(info);
  if (!impl->database.valid())
    return pg_internal("Could not connect to PostgreSQL: " + impl->database.error());
  impl->dummy_password_hash = password_hash("authentication-timing-placeholder");
  if (impl->dummy_password_hash.empty())
    return pg_internal("Password hashing initialization failed");
  auto probe = impl->database.execute("SELECT 1 FROM schema_migrations WHERE version=1");
  if (!probe.tuples_ok() || probe.rows() != 1)
    return pg_internal(
        "PostgreSQL schema is missing; apply migrations/postgresql/001-initial-schema.sql first");
  return std::unique_ptr<PostgreSQLAuthService>{new PostgreSQLAuthService{std::move(impl)}};
}
PostgreSQLAuthService::SessionResult PostgreSQLAuthService::register_user(std::string username,
                                                                          std::string password) {
  if (auto error = pg_validate(username, password))
    return *error;
  auto hash = password_hash(password);
  if (hash.empty())
    return pg_internal("Password hashing failed");
  auto result = implementation_->database.execute(
      "INSERT INTO users(username,password_hash) VALUES($1,$2) ON CONFLICT DO NOTHING RETURNING "
      "id,username,created_at,is_admin",
      {postgresql::text(username), postgresql::text(hash)});
  if (!result.tuples_ok())
    return pg_internal("Could not create user");
  if (result.rows() == 0)
    return AuthError{AuthErrorCode::duplicate_user, "Username is already registered"};
  auto user = pg_user(result);
  auto token = pg_new_token();
  auto session = implementation_->database.execute(
      "INSERT INTO sessions(user_id,token_hash,expires_at) "
      "VALUES($1,$2,clock_timestamp()+($3::integer * interval '1 second'))",
      {postgresql::number(user.id), postgresql::text(pg_token_hash(token)),
       postgresql::number(session_lifetime_seconds)});
  if (!session.command_ok())
    return pg_internal("Could not create session");
  return AuthSession{.user = std::move(user), .token = std::move(token)};
}
PostgreSQLAuthService::SessionResult PostgreSQLAuthService::login(std::string username,
                                                                  std::string password) {
  if (username.empty() || username.size() > 64 || password.empty() || password.size() > 128)
    return AuthError{AuthErrorCode::invalid_credentials, "Invalid username or password"};
  auto result =
      implementation_->database.execute("SELECT id,username,created_at,is_admin,password_hash FROM "
                                        "users WHERE lower(username)=lower($1)",
                                        {postgresql::text(username)});
  if (!result.tuples_ok())
    return pg_internal("Could not retrieve user");
  const bool exists = result.rows() == 1;
  const auto hash = exists ? result.value(0, 4) : implementation_->dummy_password_hash;
  if (crypto_pwhash_str_verify(hash.c_str(), password.data(), password.size()) != 0 || !exists)
    return AuthError{AuthErrorCode::invalid_credentials, "Invalid username or password"};
  auto user = pg_user(result);
  auto token = pg_new_token();
  auto session = implementation_->database.execute(
      "INSERT INTO sessions(user_id,token_hash,expires_at) "
      "VALUES($1,$2,clock_timestamp()+($3::integer * interval '1 second'))",
      {postgresql::number(user.id), postgresql::text(pg_token_hash(token)),
       postgresql::number(session_lifetime_seconds)});
  if (!session.command_ok())
    return pg_internal("Could not create session");
  return AuthSession{.user = std::move(user), .token = std::move(token)};
}
PostgreSQLAuthService::UserResult PostgreSQLAuthService::ensure_admin(std::string username,
                                                                      std::string password) {
  if (auto error = pg_validate(username, password))
    return *error;
  auto hash = password_hash(password);
  if (hash.empty())
    return pg_internal("Admin password hashing failed");
  auto result = implementation_->database.execute(
      "INSERT INTO users(username,password_hash,is_admin) VALUES($1,$2,true) ON "
      "CONFLICT((lower(username))) DO UPDATE SET "
      "password_hash=EXCLUDED.password_hash,is_admin=true RETURNING "
      "id,username,created_at,is_admin",
      {postgresql::text(username), postgresql::text(hash)});
  if (!result.tuples_ok())
    return pg_internal("Could not initialize admin account");
  if (result.rows() != 1)
    return pg_internal("Could not initialize admin account");
  return pg_user(result);
}
PostgreSQLAuthService::UserResult PostgreSQLAuthService::current_user(const std::string &token) {
  if (token.empty())
    return AuthError{AuthErrorCode::unauthenticated, "Authentication required"};
  auto result = implementation_->database.execute(
      "SELECT users.id,users.username,users.created_at,users.is_admin FROM sessions JOIN users ON "
      "users.id=sessions.user_id WHERE sessions.token_hash=$1 AND "
      "sessions.expires_at>clock_timestamp()",
      {postgresql::text(pg_token_hash(token))});
  if (!result.tuples_ok())
    return pg_internal("Could not retrieve session");
  if (result.rows() != 1)
    return AuthError{AuthErrorCode::unauthenticated, "Authentication required"};
  return pg_user(result);
}
std::variant<std::monostate, AuthError> PostgreSQLAuthService::logout(const std::string &token) {
  if (token.empty())
    return std::monostate{};
  auto result = implementation_->database.execute("DELETE FROM sessions WHERE token_hash=$1",
                                                  {postgresql::text(pg_token_hash(token))});
  if (!result.command_ok())
    return pg_internal("Could not invalidate session");
  return std::monostate{};
}
} // namespace algorithm_trainer
