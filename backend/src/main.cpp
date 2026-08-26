#include "algorithm-trainer/auth-service.h"
#include "algorithm-trainer/nsjail-python-executor.h"
#include "algorithm-trainer/problem-judge.h"
#include "algorithm-trainer/problem.h"
#include "algorithm-trainer/sqlite-submission-repository.h"
#include "algorithm-trainer/submission-record.h"
#include "algorithm-trainer/submission-service.h"
#include "algorithm-trainer/submission.h"

#include <drogon/drogon.h>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using ResponseCallback = std::function<void(const drogon::HttpResponsePtr &)>;
constexpr std::string_view session_cookie_name{"algorithm_trainer_session"};
constexpr int session_cookie_lifetime_seconds{60 * 60 * 24 * 30};

Json::Value user_to_json(const algorithm_trainer::AuthUser &user) {
  Json::Value body;
  body["id"] = Json::Int64{user.id};
  body["username"] = user.username;
  body["createdAt"] = user.created_at;
  return body;
}

drogon::HttpResponsePtr error_response(std::string message, drogon::HttpStatusCode status) {
  Json::Value body;
  body["error"] = std::move(message);
  auto response = drogon::HttpResponse::newHttpJsonResponse(body);
  response->setStatusCode(status);
  return response;
}

drogon::Cookie session_cookie(std::string token, bool secure, int max_age) {
  drogon::Cookie cookie{std::string{session_cookie_name}, std::move(token)};
  cookie.setPath("/");
  cookie.setHttpOnly(true);
  cookie.setSecure(secure);
  cookie.setSameSite(drogon::Cookie::SameSite::kLax);
  cookie.setMaxAge(max_age);
  return cookie;
}

void auth_result(algorithm_trainer::AuthService::SessionResult result, ResponseCallback &&callback,
                 bool secure_cookie) {
  if (const auto *session = std::get_if<algorithm_trainer::AuthSession>(&result)) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(user_to_json(session->user));
    response->addCookie(
        session_cookie(session->token, secure_cookie, session_cookie_lifetime_seconds));
    callback(response);
    return;
  }
  const auto &error = std::get<algorithm_trainer::AuthError>(result);
  if (error.code == algorithm_trainer::AuthErrorCode::internal) {
    LOG_ERROR << "Authentication operation failed: " << error.message;
    callback(
        error_response("Authentication service is unavailable", drogon::k500InternalServerError));
    return;
  }
  const auto status =
      error.code == algorithm_trainer::AuthErrorCode::invalid_credentials ? drogon::k401Unauthorized
      : error.code == algorithm_trainer::AuthErrorCode::duplicate_user    ? drogon::k409Conflict
                                                                          : drogon::k400BadRequest;
  callback(error_response(error.message, status));
}

void register_user(const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
                   algorithm_trainer::AuthService &auth_service, bool secure_cookie) {
  const auto json = request->getJsonObject();
  if (json == nullptr || !(*json)["username"].isString() || !(*json)["password"].isString()) {
    callback(error_response("Username and password are required", drogon::k400BadRequest));
    return;
  }
  auth_result(
      auth_service.register_user((*json)["username"].asString(), (*json)["password"].asString()),
      std::move(callback), secure_cookie);
}

void login(const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
           algorithm_trainer::AuthService &auth_service, bool secure_cookie) {
  const auto json = request->getJsonObject();
  if (json == nullptr || !(*json)["username"].isString() || !(*json)["password"].isString()) {
    callback(error_response("Invalid username or password", drogon::k401Unauthorized));
    return;
  }
  auth_result(auth_service.login((*json)["username"].asString(), (*json)["password"].asString()),
              std::move(callback), secure_cookie);
}

void current_user(const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
                  algorithm_trainer::AuthService &auth_service) {
  auto result = auth_service.current_user(request->getCookie(std::string{session_cookie_name}));
  if (const auto *user = std::get_if<algorithm_trainer::AuthUser>(&result)) {
    callback(drogon::HttpResponse::newHttpJsonResponse(user_to_json(*user)));
    return;
  }
  const auto &error = std::get<algorithm_trainer::AuthError>(result);
  if (error.code == algorithm_trainer::AuthErrorCode::internal) {
    LOG_ERROR << "Session lookup failed: " << error.message;
    callback(
        error_response("Authentication service is unavailable", drogon::k500InternalServerError));
    return;
  }
  callback(error_response("Authentication required", drogon::k401Unauthorized));
}

void logout(const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
            algorithm_trainer::AuthService &auth_service, bool secure_cookie) {
  auto result = auth_service.logout(request->getCookie(std::string{session_cookie_name}));
  if (const auto *error = std::get_if<algorithm_trainer::AuthError>(&result)) {
    LOG_ERROR << "Logout failed: " << error->message;
    callback(
        error_response("Authentication service is unavailable", drogon::k500InternalServerError));
    return;
  }
  Json::Value body;
  body["ok"] = true;
  auto response = drogon::HttpResponse::newHttpJsonResponse(body);
  response->addCookie(session_cookie("", secure_cookie, 0));
  callback(response);
}

Json::Value submission_to_json(const algorithm_trainer::SubmissionRecord &submission,
                               bool include_source) {
  Json::Value body;
  body["id"] = Json::Int64{submission.id.value};
  body["problemId"] = submission.problem_id;
  body["language"] = submission.language;
  body["status"] = std::string{algorithm_trainer::submission_status_name(submission.status)};
  body["createdAt"] = submission.created_at;
  if (include_source) {
    body["code"] = submission.source_code;
  }
  if (submission.verdict) {
    body["verdict"] = std::string{algorithm_trainer::verdict_name(*submission.verdict)};
  }
  if (submission.completed_at) {
    body["completedAt"] = *submission.completed_at;
  }
  if (submission.error_type) {
    body["errorType"] = *submission.error_type;
  }
  return body;
}

void get_problem(const drogon::HttpRequestPtr &, ResponseCallback &&callback,
                 const std::string &slug) {
  const auto *problem = algorithm_trainer::find_problem(slug);
  if (problem == nullptr) {
    Json::Value body;
    body["error"] = "Problem not found";
    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(drogon::k404NotFound);
    callback(response);
    return;
  }

  callback(drogon::HttpResponse::newHttpJsonResponse(algorithm_trainer::problem_to_json(*problem)));
}

void list_problems(const drogon::HttpRequestPtr &, ResponseCallback &&callback) {
  Json::Value body{Json::arrayValue};
  for (const auto &problem : algorithm_trainer::all_problems()) {
    body.append(algorithm_trainer::problem_summary_to_json(problem));
  }
  callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void submit(const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
            algorithm_trainer::SubmissionService &submission_service,
            algorithm_trainer::AuthService &auth_service) {
  const auto json = request->getJsonObject();
  if (json == nullptr) {
    Json::Value body;
    body["error"] = "Request body must contain valid JSON";
    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(drogon::k400BadRequest);
    callback(response);
    return;
  }

  const auto validation = algorithm_trainer::validate_submission(*json);
  if (const auto *error = std::get_if<algorithm_trainer::ValidationError>(&validation)) {
    Json::Value body;
    body["error"] = error->message;
    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(drogon::k400BadRequest);
    callback(response);
    return;
  }

  auto submission = std::get<algorithm_trainer::SubmissionRequest>(validation);
  const auto token = request->getCookie(std::string{session_cookie_name});
  if (!token.empty()) {
    auto user = auth_service.current_user(token);
    if (const auto *authenticated = std::get_if<algorithm_trainer::AuthUser>(&user)) {
      submission.user_id = authenticated->id;
    } else if (std::get<algorithm_trainer::AuthError>(user).code ==
               algorithm_trainer::AuthErrorCode::internal) {
      LOG_ERROR << "Could not resolve submission owner";
      callback(error_response("Submission could not be accepted", drogon::k500InternalServerError));
      return;
    }
  }
  auto result = submission_service.submit(submission);
  if (const auto *submission = std::get_if<algorithm_trainer::SubmissionRecord>(&result)) {
    callback(drogon::HttpResponse::newHttpJsonResponse(submission_to_json(*submission, false)));
    return;
  }

  const auto &error = std::get<algorithm_trainer::SubmissionServiceError>(result);
  LOG_ERROR << "Submission failed: " << error.message;

  Json::Value body;
  body["error"] = "Submission could not be judged";
  auto response = drogon::HttpResponse::newHttpJsonResponse(body);
  response->setStatusCode(drogon::k500InternalServerError);
  callback(response);
}

void submission_history(const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
                        const std::string &problem_id,
                        algorithm_trainer::SubmissionService &submission_service,
                        algorithm_trainer::AuthService &auth_service) {
  if (algorithm_trainer::find_problem(problem_id) == nullptr) {
    callback(error_response("Problem not found", drogon::k404NotFound));
    return;
  }
  auto authenticated =
      auth_service.current_user(request->getCookie(std::string{session_cookie_name}));
  if (const auto *error = std::get_if<algorithm_trainer::AuthError>(&authenticated)) {
    if (error->code == algorithm_trainer::AuthErrorCode::internal) {
      LOG_ERROR << "Submission history authentication failed: " << error->message;
      callback(
          error_response("Authentication service is unavailable", drogon::k500InternalServerError));
    } else {
      callback(error_response("Authentication required", drogon::k401Unauthorized));
    }
    return;
  }

  auto history = submission_service.history(std::get<algorithm_trainer::AuthUser>(authenticated).id,
                                            problem_id);
  if (const auto *error = std::get_if<algorithm_trainer::SubmissionServiceError>(&history)) {
    LOG_ERROR << "Submission history lookup failed: " << error->message;
    callback(error_response("Submission history could not be retrieved",
                            drogon::k500InternalServerError));
    return;
  }
  Json::Value body{Json::arrayValue};
  for (const auto &record : std::get<std::vector<algorithm_trainer::SubmissionRecord>>(history)) {
    body.append(submission_to_json(record, true));
  }
  callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void profile(const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
             algorithm_trainer::SubmissionService &submission_service,
             algorithm_trainer::AuthService &auth_service) {
  auto authenticated =
      auth_service.current_user(request->getCookie(std::string{session_cookie_name}));
  if (const auto *error = std::get_if<algorithm_trainer::AuthError>(&authenticated)) {
    if (error->code == algorithm_trainer::AuthErrorCode::internal) {
      LOG_ERROR << "Profile authentication failed: " << error->message;
      callback(
          error_response("Authentication service is unavailable", drogon::k500InternalServerError));
    } else {
      callback(error_response("Authentication required", drogon::k401Unauthorized));
    }
    return;
  }

  const auto &user = std::get<algorithm_trainer::AuthUser>(authenticated);
  auto progress_result = submission_service.progress(user.id);
  if (const auto *error =
          std::get_if<algorithm_trainer::SubmissionServiceError>(&progress_result)) {
    LOG_ERROR << "Profile progress lookup failed: " << error->message;
    callback(error_response("Profile could not be retrieved", drogon::k500InternalServerError));
    return;
  }
  const auto &progress = std::get<algorithm_trainer::UserProgress>(progress_result);

  Json::Value body;
  body["user"] = user_to_json(user);
  body["activity"]["totalSubmissions"] = Json::Int64{progress.total_submissions};
  body["activity"]["acceptedSubmissions"] = Json::Int64{progress.accepted_submissions};
  body["activity"]["currentStreakDays"] = Json::Int64{progress.current_streak_days};
  body["activity"]["completedProblems"] =
      Json::Int64{static_cast<std::int64_t>(progress.completed_problems.size())};
  if (progress.most_recent_submission_problem_id) {
    body["mostRecentSubmissionProblemId"] = *progress.most_recent_submission_problem_id;
  } else {
    body["mostRecentSubmissionProblemId"] = Json::nullValue;
  }
  body["completedProblems"] = Json::Value{Json::arrayValue};
  for (const auto &completed : progress.completed_problems) {
    Json::Value item;
    item["id"] = completed.problem_id;
    const auto *problem = algorithm_trainer::find_problem(completed.problem_id);
    item["title"] = problem == nullptr ? completed.problem_id : problem->title;
    item["completedAt"] = completed.completed_at;
    body["completedProblems"].append(std::move(item));
  }
  callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void get_submission(const drogon::HttpRequestPtr &, ResponseCallback &&callback,
                    const std::string &raw_id,
                    algorithm_trainer::SubmissionService &submission_service) {
  std::int64_t value{};
  const auto [end, error] = std::from_chars(raw_id.data(), raw_id.data() + raw_id.size(), value);
  if (error != std::errc{} || end != raw_id.data() + raw_id.size() || value <= 0) {
    Json::Value body;
    body["error"] = "Submission id must be a positive integer";
    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(drogon::k400BadRequest);
    callback(response);
    return;
  }

  auto result = submission_service.find(algorithm_trainer::SubmissionId{value});
  if (const auto *service_error = std::get_if<algorithm_trainer::SubmissionServiceError>(&result)) {
    LOG_ERROR << "Submission lookup failed: " << service_error->message;
    Json::Value body;
    body["error"] = "Submission could not be retrieved";
    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(drogon::k500InternalServerError);
    callback(response);
    return;
  }

  const auto &submission = std::get<std::optional<algorithm_trainer::SubmissionRecord>>(result);
  if (!submission) {
    Json::Value body;
    body["error"] = "Submission not found";
    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(drogon::k404NotFound);
    callback(response);
    return;
  }

  callback(drogon::HttpResponse::newHttpJsonResponse(submission_to_json(*submission, true)));
}

} // namespace

int main() {
  constexpr std::uint16_t port{8080};
  algorithm_trainer::NsJailPythonExecutor executor;
  algorithm_trainer::ProblemJudge judge{executor};
  const auto *configured_database = std::getenv("ALGORITHM_TRAINER_DATABASE");
  const auto database_path = configured_database == nullptr
                                 ? std::filesystem::path{"data/algorithm-trainer.sqlite3"}
                                 : std::filesystem::path{configured_database};
  auto repository_result = algorithm_trainer::SQLiteSubmissionRepository::open(database_path);
  if (const auto *error = std::get_if<algorithm_trainer::RepositoryError>(&repository_result)) {
    LOG_ERROR << "Database initialization failed: " << error->message;
    return 1;
  }
  auto repository = std::get<std::unique_ptr<algorithm_trainer::SQLiteSubmissionRepository>>(
      std::move(repository_result));
  algorithm_trainer::SubmissionService submission_service{judge, *repository};
  auto auth_result = algorithm_trainer::AuthService::open(database_path);
  if (const auto *error = std::get_if<algorithm_trainer::AuthError>(&auth_result)) {
    LOG_ERROR << "Authentication initialization failed: " << error->message;
    return 1;
  }
  auto auth_service =
      std::get<std::unique_ptr<algorithm_trainer::AuthService>>(std::move(auth_result));
  const auto *secure_cookie_setting = std::getenv("ALGORITHM_TRAINER_SECURE_COOKIES");
  const bool secure_cookies =
      secure_cookie_setting != nullptr && std::string_view{secure_cookie_setting} == "1";

  drogon::app()
      .registerHandler("/api/auth/register",
                       [&auth_service, secure_cookies](const drogon::HttpRequestPtr &request,
                                                       ResponseCallback &&callback) {
                         register_user(request, std::move(callback), *auth_service, secure_cookies);
                       },
                       {drogon::Post})
      .registerHandler("/api/auth/login",
                       [&auth_service, secure_cookies](const drogon::HttpRequestPtr &request,
                                                       ResponseCallback &&callback) {
                         login(request, std::move(callback), *auth_service, secure_cookies);
                       },
                       {drogon::Post})
      .registerHandler("/api/auth/logout",
                       [&auth_service, secure_cookies](const drogon::HttpRequestPtr &request,
                                                       ResponseCallback &&callback) {
                         logout(request, std::move(callback), *auth_service, secure_cookies);
                       },
                       {drogon::Post})
      .registerHandler(
          "/api/auth/me",
          [&auth_service](const drogon::HttpRequestPtr &request, ResponseCallback &&callback) {
            current_user(request, std::move(callback), *auth_service);
          },
          {drogon::Get})
      .registerHandler("/api/profile",
                       [&submission_service, &auth_service](const drogon::HttpRequestPtr &request,
                                                            ResponseCallback &&callback) {
                         profile(request, std::move(callback), submission_service, *auth_service);
                       },
                       {drogon::Get})
      .registerHandler("/api/problems",
                       [](const drogon::HttpRequestPtr &request, ResponseCallback &&callback) {
                         list_problems(request, std::move(callback));
                       },
                       {drogon::Get})
      .registerHandler(
          "/api/problems/{1}",
          [](const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
             const std::string &slug) { get_problem(request, std::move(callback), slug); },
          {drogon::Get})
      .registerHandler("/api/problems/{1}/submissions",
                       [&submission_service, &auth_service](const drogon::HttpRequestPtr &request,
                                                            ResponseCallback &&callback,
                                                            const std::string &problem_id) {
                         submission_history(request, std::move(callback), problem_id,
                                            submission_service, *auth_service);
                       },
                       {drogon::Get})
      .registerHandler("/api/submissions/{1}",
                       [&submission_service](const drogon::HttpRequestPtr &request,
                                             ResponseCallback &&callback, const std::string &id) {
                         get_submission(request, std::move(callback), id, submission_service);
                       },
                       {drogon::Get})
      .registerHandler("/api/submissions",
                       [&submission_service, &auth_service](const drogon::HttpRequestPtr &request,
                                                            ResponseCallback &&callback) {
                         submit(request, std::move(callback), submission_service, *auth_service);
                       },
                       {drogon::Post})
      .addListener("127.0.0.1", port)
      .run();
}
