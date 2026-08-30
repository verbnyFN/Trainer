#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <variant>

namespace algorithm_trainer {

struct AuthUser {
  std::int64_t id{};
  std::string username;
  std::string created_at;
  bool is_admin{};
};

[[nodiscard]] inline bool has_admin_access(const AuthUser &user) { return user.is_admin; }

enum class AuthErrorCode {
  invalid_input,
  duplicate_user,
  invalid_credentials,
  unauthenticated,
  internal,
};

struct AuthError {
  AuthErrorCode code;
  std::string message;
};

struct AuthSession {
  AuthUser user;
  std::string token;
};

class AuthProvider {
public:
  virtual ~AuthProvider() = default;
  virtual std::variant<AuthSession, AuthError> register_user(std::string, std::string) = 0;
  virtual std::variant<AuthSession, AuthError> login(std::string, std::string) = 0;
  virtual std::variant<AuthUser, AuthError> ensure_admin(std::string, std::string) = 0;
  virtual std::variant<AuthUser, AuthError> current_user(const std::string &) = 0;
  virtual std::variant<std::monostate, AuthError> logout(const std::string &) = 0;
};

class AuthService final : public AuthProvider {
public:
  using OpenResult = std::variant<std::unique_ptr<AuthService>, AuthError>;
  using UserResult = std::variant<AuthUser, AuthError>;
  using SessionResult = std::variant<AuthSession, AuthError>;

  ~AuthService() override;
  AuthService(const AuthService &) = delete;
  AuthService &operator=(const AuthService &) = delete;

  [[nodiscard]] static OpenResult open(const std::filesystem::path &database_path);
  [[nodiscard]] SessionResult register_user(std::string username, std::string password) override;
  [[nodiscard]] SessionResult login(std::string username, std::string password) override;
  [[nodiscard]] UserResult ensure_admin(std::string username, std::string password) override;
  [[nodiscard]] UserResult current_user(const std::string &token) override;
  [[nodiscard]] std::variant<std::monostate, AuthError> logout(const std::string &token) override;

private:
  struct Implementation;
  explicit AuthService(std::unique_ptr<Implementation> implementation);

  std::unique_ptr<Implementation> implementation_;
};

class PostgreSQLAuthService final : public AuthProvider {
public:
  using OpenResult = std::variant<std::unique_ptr<PostgreSQLAuthService>, AuthError>;
  using UserResult = std::variant<AuthUser, AuthError>;
  using SessionResult = std::variant<AuthSession, AuthError>;

  ~PostgreSQLAuthService() override;
  [[nodiscard]] static OpenResult open(const std::string &connection_info);
  [[nodiscard]] SessionResult register_user(std::string username, std::string password) override;
  [[nodiscard]] SessionResult login(std::string username, std::string password) override;
  [[nodiscard]] UserResult ensure_admin(std::string username, std::string password) override;
  [[nodiscard]] UserResult current_user(const std::string &token) override;
  [[nodiscard]] std::variant<std::monostate, AuthError> logout(const std::string &token) override;

private:
  struct Implementation;
  explicit PostgreSQLAuthService(std::unique_ptr<Implementation> implementation);
  std::unique_ptr<Implementation> implementation_;
};

} // namespace algorithm_trainer
