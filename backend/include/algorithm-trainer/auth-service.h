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
};

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

class AuthService final {
public:
  using OpenResult = std::variant<std::unique_ptr<AuthService>, AuthError>;
  using UserResult = std::variant<AuthUser, AuthError>;
  using SessionResult = std::variant<AuthSession, AuthError>;

  ~AuthService();
  AuthService(const AuthService &) = delete;
  AuthService &operator=(const AuthService &) = delete;

  [[nodiscard]] static OpenResult open(const std::filesystem::path &database_path);
  [[nodiscard]] SessionResult register_user(std::string username, std::string password);
  [[nodiscard]] SessionResult login(std::string username, std::string password);
  [[nodiscard]] UserResult current_user(const std::string &token);
  [[nodiscard]] std::variant<std::monostate, AuthError> logout(const std::string &token);

private:
  struct Implementation;
  explicit AuthService(std::unique_ptr<Implementation> implementation);

  std::unique_ptr<Implementation> implementation_;
};

} // namespace algorithm_trainer
