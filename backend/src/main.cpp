#include "algorithm-trainer/admin-validation.h"
#include "algorithm-trainer/auth-service.h"
#include "algorithm-trainer/nsjail-python-executor.h"
#include "algorithm-trainer/problem-judge.h"
#include "algorithm-trainer/problem-service.h"
#include "algorithm-trainer/problem.h"
#include "algorithm-trainer/sqlite-problem-repository.h"
#include "algorithm-trainer/sqlite-submission-repository.h"
#include "algorithm-trainer/submission-record.h"
#include "algorithm-trainer/submission-service.h"
#include "algorithm-trainer/submission-worker.h"
#include "algorithm-trainer/submission.h"

#include <drogon/drogon.h>

#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
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
  body["isAdmin"] = user.is_admin;
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

Json::Value admin_submission_to_json(const algorithm_trainer::SubmissionRecord &submission) {
  auto body = submission_to_json(submission, true);
  if (submission.user_id)
    body["userId"] = Json::Int64{*submission.user_id};
  else
    body["userId"] = Json::nullValue;
  if (submission.retry_of)
    body["retryOf"] = Json::Int64{submission.retry_of->value};
  Json::Value diagnostic;
  diagnostic["category"] = submission.error_type.value_or(
      submission.verdict
          ? std::string{algorithm_trainer::verdict_name(*submission.verdict)}
          : std::string{algorithm_trainer::submission_status_name(submission.status)});
  diagnostic["message"] = submission.error_type ? "Execution ended with a normalized runtime error."
                          : submission.status == algorithm_trainer::SubmissionStatus::failed
                              ? "Judging infrastructure could not complete this submission."
                              : "No private diagnostic details are available.";
  body["diagnostic"] = std::move(diagnostic);
  return body;
}

void get_problem(const drogon::HttpRequestPtr &, ResponseCallback &&callback,
                 const std::string &slug, algorithm_trainer::ProblemService &problems) {
  auto result = problems.find(slug);
  if (const auto *error = std::get_if<algorithm_trainer::ProblemRepositoryError>(&result)) {
    LOG_ERROR << "Problem lookup failed: " << error->message;
    callback(error_response("Problem could not be retrieved", drogon::k500InternalServerError));
    return;
  }
  auto problem = std::get<std::optional<algorithm_trainer::Problem>>(std::move(result));
  if (!problem) {
    Json::Value body;
    body["error"] = "Problem not found";
    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(drogon::k404NotFound);
    callback(response);
    return;
  }

  callback(drogon::HttpResponse::newHttpJsonResponse(algorithm_trainer::problem_to_json(*problem)));
}

void list_problems(const drogon::HttpRequestPtr &, ResponseCallback &&callback,
                   algorithm_trainer::ProblemService &problems) {
  auto result = problems.list();
  if (const auto *error = std::get_if<algorithm_trainer::ProblemRepositoryError>(&result)) {
    LOG_ERROR << "Problem list failed: " << error->message;
    callback(error_response("Problems could not be retrieved", drogon::k500InternalServerError));
    return;
  }
  Json::Value body{Json::arrayValue};
  for (const auto &problem : std::get<std::vector<algorithm_trainer::Problem>>(result)) {
    body.append(algorithm_trainer::problem_summary_to_json(problem));
  }
  callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void submit(const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
            algorithm_trainer::SubmissionService &submission_service,
            algorithm_trainer::AuthService &auth_service,
            algorithm_trainer::ProblemService &problems) {
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
  auto problem_result = problems.find(submission.problem_id);
  if (const auto *error = std::get_if<algorithm_trainer::ProblemRepositoryError>(&problem_result)) {
    LOG_ERROR << "Submission problem validation failed: " << error->message;
    callback(error_response("Submission could not be accepted", drogon::k500InternalServerError));
    return;
  }
  auto problem = std::get<std::optional<algorithm_trainer::Problem>>(std::move(problem_result));
  if (!problem) {
    callback(error_response("Unsupported problemId", drogon::k400BadRequest));
    return;
  }
  if (std::ranges::find(problem->languages, submission.language) == problem->languages.end()) {
    callback(error_response("Unsupported language", drogon::k400BadRequest));
    return;
  }
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
    auto response =
        drogon::HttpResponse::newHttpJsonResponse(submission_to_json(*submission, false));
    response->setStatusCode(drogon::k202Accepted);
    callback(response);
    return;
  }

  const auto &error = std::get<algorithm_trainer::SubmissionServiceError>(result);
  if (error.code == algorithm_trainer::SubmissionServiceErrorCode::rate_limited ||
      error.code == algorithm_trainer::SubmissionServiceErrorCode::queue_full) {
    auto response = error_response(error.message, drogon::k429TooManyRequests);
    response->addHeader("Retry-After", "2");
    callback(response);
    return;
  }
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
                        algorithm_trainer::AuthService &auth_service,
                        algorithm_trainer::ProblemService &problems) {
  auto problem = problems.find(problem_id);
  if (std::holds_alternative<algorithm_trainer::ProblemRepositoryError>(problem)) {
    callback(error_response("Problem could not be retrieved", drogon::k500InternalServerError));
    return;
  }
  if (!std::get<std::optional<algorithm_trainer::Problem>>(problem)) {
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
             algorithm_trainer::AuthService &auth_service,
             algorithm_trainer::ProblemService &problems) {
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
    auto found = problems.find_for_judging(completed.problem_id);
    if (const auto *problem = std::get_if<std::optional<algorithm_trainer::Problem>>(&found);
        problem != nullptr && problem->has_value()) {
      item["title"] = problem->value().title;
    } else {
      item["title"] = completed.problem_id;
    }
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

bool require_admin(const drogon::HttpRequestPtr &request, ResponseCallback &callback,
                   algorithm_trainer::AuthService &auth_service) {
  auto result = auth_service.current_user(request->getCookie(std::string{session_cookie_name}));
  if (const auto *user = std::get_if<algorithm_trainer::AuthUser>(&result)) {
    if (algorithm_trainer::has_admin_access(*user))
      return true;
    callback(error_response("Administrator access required", drogon::k403Forbidden));
    return false;
  }
  const auto &error = std::get<algorithm_trainer::AuthError>(result);
  if (error.code == algorithm_trainer::AuthErrorCode::internal) {
    LOG_ERROR << "Admin authorization failed: " << error.message;
    callback(
        error_response("Authentication service is unavailable", drogon::k500InternalServerError));
  } else {
    callback(error_response("Authentication required", drogon::k401Unauthorized));
  }
  return false;
}

std::int64_t authenticated_admin_id(const drogon::HttpRequestPtr &request,
                                    algorithm_trainer::AuthService &auth_service) {
  const auto result =
      auth_service.current_user(request->getCookie(std::string{session_cookie_name}));
  return std::get<algorithm_trainer::AuthUser>(result).id;
}

void audit_admin_change(algorithm_trainer::SubmissionRepository &repository, std::int64_t admin_id,
                        const std::string &action, const std::string &entity_type,
                        const std::string &entity_id) {
  if (const auto result = repository.audit(admin_id, action, entity_type, entity_id);
      const auto *error = std::get_if<algorithm_trainer::RepositoryError>(&result))
    LOG_ERROR << "Administrative audit write failed: " << error->message;
}

std::optional<std::int64_t> positive_id(const std::string &raw) {
  std::int64_t value{};
  const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
  if (error != std::errc{} || end != raw.data() + raw.size() || value <= 0)
    return std::nullopt;
  return value;
}

std::variant<algorithm_trainer::Problem, std::string>
parse_admin_problem(const Json::Value &json, std::optional<std::string> path_id = std::nullopt) {
  if (!path_id && !json["id"].isString())
    return "Problem id must be a string";
  const auto id = path_id ? *path_id : json["id"].asString();
  if (id.empty() || !json["title"].isString() || json["title"].asString().empty() ||
      !json["description"].isString() || !json["inputFormat"].isString() ||
      !json["outputFormat"].isString() || !json["difficulty"].isString() ||
      !json["tags"].isArray() || !json["languages"].isArray() || !json["examples"].isArray() ||
      !json["enabled"].isBool())
    return "Problem fields are invalid or incomplete";
  if (!algorithm_trainer::valid_problem_slug(id))
    return "Problem id must be a lowercase slug";
  if (json["title"].asString().size() > 200 ||
      !algorithm_trainer::valid_problem_text(json["description"].asString()) ||
      !algorithm_trainer::valid_problem_text(json["inputFormat"].asString()) ||
      !algorithm_trainer::valid_problem_text(json["outputFormat"].asString()) ||
      json["tags"].size() > 50 || json["languages"].size() > 10 || json["examples"].size() > 20)
    return "Problem metadata exceeds the allowed size";
  algorithm_trainer::Problem problem{.id = id,
                                     .title = json["title"].asString(),
                                     .description = json["description"].asString(),
                                     .input_format = json["inputFormat"].asString(),
                                     .output_format = json["outputFormat"].asString(),
                                     .enabled = json["enabled"].asBool()};
  if (path_id) {
    if (!json["revision"].isInt64() || json["revision"].asInt64() <= 0)
      return "A positive revision is required when updating a problem";
    problem.revision = json["revision"].asInt64();
  }
  const auto difficulty = json["difficulty"].asString();
  if (difficulty == "Easy")
    problem.difficulty = algorithm_trainer::ProblemDifficulty::easy;
  else if (difficulty == "Medium")
    problem.difficulty = algorithm_trainer::ProblemDifficulty::medium;
  else if (difficulty == "Hard")
    problem.difficulty = algorithm_trainer::ProblemDifficulty::hard;
  else
    return "Difficulty must be Easy, Medium, or Hard";
  for (const auto &tag : json["tags"]) {
    if (!tag.isString())
      return "Tags must contain strings";
    problem.tags.push_back(tag.asString());
  }
  for (const auto &language : json["languages"]) {
    if (!language.isString())
      return "Languages must contain strings";
    problem.languages.push_back(language.asString());
  }
  for (const auto &example : json["examples"]) {
    if (!example.isObject() || !example["input"].isString() || !example["output"].isString())
      return "Examples must contain input and output strings";
    problem.examples.push_back({example["input"].asString(), example["output"].asString()});
  }
  return problem;
}

std::variant<algorithm_trainer::ProblemTestCase, std::string>
parse_admin_test(const Json::Value &json, std::int64_t id = 0) {
  if (!json["input"].isString() || !json["expectedOutput"].isString() ||
      !json["position"].isInt64() || json["position"].asInt64() < 0 || !json["enabled"].isBool())
    return "Test input, expectedOutput, non-negative position, and enabled are required";
  if (!algorithm_trainer::valid_hidden_test_text(json["input"].asString()) ||
      !algorithm_trainer::valid_hidden_test_text(json["expectedOutput"].asString()))
    return "Hidden-test input or output exceeds the allowed size";
  if (id > 0 && (!json["revision"].isInt64() || json["revision"].asInt64() <= 0))
    return "A positive revision is required when updating a hidden test";
  return algorithm_trainer::ProblemTestCase{.input = json["input"].asString(),
                                            .expected_output = json["expectedOutput"].asString(),
                                            .id = id,
                                            .position = json["position"].asInt64(),
                                            .enabled = json["enabled"].asBool(),
                                            .revision = id > 0 ? json["revision"].asInt64() : 1};
}

void admin_list_problems(const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
                         algorithm_trainer::AuthService &auth,
                         algorithm_trainer::ProblemRepository &repository) {
  if (!require_admin(request, callback, auth))
    return;
  auto result = repository.list_all();
  if (const auto *error = std::get_if<algorithm_trainer::ProblemRepositoryError>(&result)) {
    LOG_ERROR << "Admin problem list failed: " << error->message;
    callback(error_response("Problems could not be retrieved", drogon::k500InternalServerError));
    return;
  }
  Json::Value body{Json::arrayValue};
  for (const auto &problem : std::get<std::vector<algorithm_trainer::Problem>>(result)) {
    auto item = algorithm_trainer::problem_summary_to_json(problem);
    item["enabled"] = problem.enabled;
    body.append(std::move(item));
  }
  callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void admin_get_problem(const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
                       const std::string &id, algorithm_trainer::AuthService &auth,
                       algorithm_trainer::ProblemRepository &repository) {
  if (!require_admin(request, callback, auth))
    return;
  auto result = repository.find_any(id);
  if (const auto *error = std::get_if<algorithm_trainer::ProblemRepositoryError>(&result)) {
    LOG_ERROR << "Admin problem lookup failed: " << error->message;
    callback(error_response("Problem could not be retrieved", drogon::k500InternalServerError));
    return;
  }
  const auto &problem = std::get<std::optional<algorithm_trainer::Problem>>(result);
  if (!problem) {
    callback(error_response("Problem not found", drogon::k404NotFound));
    return;
  }
  callback(drogon::HttpResponse::newHttpJsonResponse(
      algorithm_trainer::admin_problem_to_json(*problem)));
}

void admin_write_problem(const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
                         std::optional<std::string> id, algorithm_trainer::AuthService &auth,
                         algorithm_trainer::ProblemRepository &repository,
                         algorithm_trainer::SubmissionRepository &audit_repository) {
  if (!require_admin(request, callback, auth))
    return;
  const auto json = request->getJsonObject();
  if (!json) {
    callback(error_response("Request body must contain valid JSON", drogon::k400BadRequest));
    return;
  }
  auto parsed = parse_admin_problem(*json, id);
  if (const auto *error = std::get_if<std::string>(&parsed)) {
    callback(error_response(*error, drogon::k400BadRequest));
    return;
  }
  auto result = id ? repository.update(std::get<algorithm_trainer::Problem>(parsed))
                   : repository.create(std::get<algorithm_trainer::Problem>(parsed));
  if (const auto *error = std::get_if<algorithm_trainer::ProblemRepositoryError>(&result)) {
    LOG_WARN << "Admin problem write failed: " << error->message;
    callback(error_response(error->message, error->message == "Problem not found"
                                                ? drogon::k404NotFound
                                                : drogon::k409Conflict));
    return;
  }
  auto response = drogon::HttpResponse::newHttpJsonResponse(
      algorithm_trainer::admin_problem_to_json(std::get<algorithm_trainer::Problem>(result)));
  audit_admin_change(audit_repository, authenticated_admin_id(request, auth),
                     id ? "problem.update" : "problem.create", "problem",
                     std::get<algorithm_trainer::Problem>(result).id);
  if (!id)
    response->setStatusCode(drogon::k201Created);
  callback(response);
}

void admin_delete_problem(const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
                          const std::string &id, algorithm_trainer::AuthService &auth,
                          algorithm_trainer::ProblemRepository &repository,
                          algorithm_trainer::SubmissionRepository &audit_repository) {
  if (!require_admin(request, callback, auth))
    return;
  auto result = repository.remove(id);
  if (const auto *error = std::get_if<algorithm_trainer::ProblemRepositoryError>(&result)) {
    LOG_WARN << "Admin problem deletion failed: " << error->message;
    callback(error_response("Problem could not be deleted", drogon::k409Conflict));
  } else if (!std::get<bool>(result)) {
    callback(error_response("Problem not found", drogon::k404NotFound));
  } else {
    audit_admin_change(audit_repository, authenticated_admin_id(request, auth), "problem.delete",
                       "problem", id);
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k204NoContent);
    callback(response);
  }
}

void admin_write_test(const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
                      const std::string &problem_id, std::optional<std::string> raw_test_id,
                      algorithm_trainer::AuthService &auth,
                      algorithm_trainer::ProblemRepository &repository,
                      algorithm_trainer::SubmissionRepository &audit_repository) {
  if (!require_admin(request, callback, auth))
    return;
  std::int64_t test_id{};
  if (raw_test_id) {
    const auto parsed_id = positive_id(*raw_test_id);
    if (!parsed_id) {
      callback(error_response("Test id must be a positive integer", drogon::k400BadRequest));
      return;
    }
    test_id = *parsed_id;
  }
  const auto json = request->getJsonObject();
  if (!json) {
    callback(error_response("Request body must contain valid JSON", drogon::k400BadRequest));
    return;
  }
  auto parsed = parse_admin_test(*json, test_id);
  if (const auto *error = std::get_if<std::string>(&parsed)) {
    callback(error_response(*error, drogon::k400BadRequest));
    return;
  }
  auto result =
      raw_test_id
          ? repository.update_test(problem_id, std::get<algorithm_trainer::ProblemTestCase>(parsed))
          : repository.create_test(problem_id,
                                   std::get<algorithm_trainer::ProblemTestCase>(parsed));
  if (const auto *error = std::get_if<algorithm_trainer::ProblemRepositoryError>(&result)) {
    LOG_WARN << "Admin hidden-test write failed: " << error->message;
    callback(error_response(error->message, drogon::k409Conflict));
    return;
  }
  auto response =
      drogon::HttpResponse::newHttpJsonResponse(algorithm_trainer::admin_problem_test_to_json(
          std::get<algorithm_trainer::ProblemTestCase>(result)));
  audit_admin_change(audit_repository, authenticated_admin_id(request, auth),
                     raw_test_id ? "problem-test.update" : "problem-test.create", "problem-test",
                     problem_id + ":" +
                         std::to_string(std::get<algorithm_trainer::ProblemTestCase>(result).id));
  if (!raw_test_id)
    response->setStatusCode(drogon::k201Created);
  callback(response);
}

void admin_delete_test(const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
                       const std::string &problem_id, const std::string &raw_test_id,
                       algorithm_trainer::AuthService &auth,
                       algorithm_trainer::ProblemRepository &repository,
                       algorithm_trainer::SubmissionRepository &audit_repository) {
  if (!require_admin(request, callback, auth))
    return;
  const auto test_id = positive_id(raw_test_id);
  if (!test_id) {
    callback(error_response("Test id must be a positive integer", drogon::k400BadRequest));
    return;
  }
  auto result = repository.remove_test(problem_id, *test_id);
  if (const auto *error = std::get_if<algorithm_trainer::ProblemRepositoryError>(&result)) {
    LOG_WARN << "Admin hidden-test deletion failed: " << error->message;
    callback(error_response("Hidden test could not be deleted", drogon::k409Conflict));
  } else if (!std::get<bool>(result)) {
    callback(error_response("Hidden test not found", drogon::k404NotFound));
  } else {
    audit_admin_change(audit_repository, authenticated_admin_id(request, auth),
                       "problem-test.delete", "problem-test",
                       problem_id + ":" + std::to_string(*test_id));
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k204NoContent);
    callback(response);
  }
}

void admin_submissions(const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
                       std::optional<std::string> raw_id, algorithm_trainer::AuthService &auth,
                       algorithm_trainer::SubmissionRepository &repository) {
  if (!require_admin(request, callback, auth))
    return;
  if (raw_id) {
    const auto id = positive_id(*raw_id);
    if (!id) {
      callback(error_response("Submission id must be a positive integer", drogon::k400BadRequest));
      return;
    }
    auto result = repository.find(algorithm_trainer::SubmissionId{*id});
    if (const auto *error = std::get_if<algorithm_trainer::RepositoryError>(&result)) {
      LOG_ERROR << "Admin submission lookup failed: " << error->message;
      callback(
          error_response("Submission could not be retrieved", drogon::k500InternalServerError));
      return;
    }
    const auto &record = std::get<std::optional<algorithm_trainer::SubmissionRecord>>(result);
    if (!record) {
      callback(error_response("Submission not found", drogon::k404NotFound));
      return;
    }
    callback(drogon::HttpResponse::newHttpJsonResponse(admin_submission_to_json(*record)));
    return;
  }
  algorithm_trainer::SubmissionAdminFilter filter;
  const auto optional_parameter =
      [&request](const std::string &name) -> std::optional<std::string> {
    auto value = request->getParameter(name);
    return value.empty() ? std::nullopt : std::optional<std::string>{std::move(value)};
  };
  if (const auto status = optional_parameter("status")) {
    if (*status == "Failed")
      filter.status = algorithm_trainer::SubmissionStatus::failed;
    else if (*status == "Queued")
      filter.status = algorithm_trainer::SubmissionStatus::queued;
    else if (*status == "Running")
      filter.status = algorithm_trainer::SubmissionStatus::running;
    else if (*status == "Completed")
      filter.status = algorithm_trainer::SubmissionStatus::completed;
    else {
      callback(error_response("Unknown submission status filter", drogon::k400BadRequest));
      return;
    }
  }
  filter.error_type = optional_parameter("errorType");
  filter.language = optional_parameter("language");
  filter.problem_id = optional_parameter("problemId");
  filter.created_from = optional_parameter("from");
  filter.created_to = optional_parameter("to");
  if (const auto user_id = optional_parameter("userId")) {
    const auto parsed = positive_id(*user_id);
    if (!parsed) {
      callback(error_response("User id must be a positive integer", drogon::k400BadRequest));
      return;
    }
    filter.user_id = *parsed;
  }
  auto result = repository.list_filtered(filter);
  if (const auto *error = std::get_if<algorithm_trainer::RepositoryError>(&result)) {
    LOG_ERROR << "Admin submission list failed: " << error->message;
    callback(error_response("Submissions could not be retrieved", drogon::k500InternalServerError));
    return;
  }
  Json::Value body{Json::arrayValue};
  for (const auto &record : std::get<std::vector<algorithm_trainer::SubmissionRecord>>(result))
    body.append(admin_submission_to_json(record));
  callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void admin_retry_submission(const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
                            const std::string &raw_id, algorithm_trainer::AuthService &auth,
                            algorithm_trainer::SubmissionRepository &repository,
                            const std::function<void()> &notify_workers) {
  if (!require_admin(request, callback, auth))
    return;
  const auto id = positive_id(raw_id);
  if (!id) {
    callback(error_response("Submission id must be a positive integer", drogon::k400BadRequest));
    return;
  }
  auto result =
      repository.retry(algorithm_trainer::SubmissionId{*id}, authenticated_admin_id(request, auth));
  if (const auto *error = std::get_if<algorithm_trainer::RepositoryError>(&result)) {
    callback(error_response(error->message, drogon::k409Conflict));
    return;
  }
  notify_workers();
  auto response = drogon::HttpResponse::newHttpJsonResponse(
      admin_submission_to_json(std::get<algorithm_trainer::SubmissionRecord>(result)));
  response->setStatusCode(drogon::k202Accepted);
  callback(response);
}

} // namespace

int main() {
  constexpr std::uint16_t port{8080};
  algorithm_trainer::NsJailPythonExecutor executor;
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
  auto problem_repository_result = algorithm_trainer::SQLiteProblemRepository::open(database_path);
  if (const auto *error =
          std::get_if<algorithm_trainer::ProblemRepositoryError>(&problem_repository_result)) {
    LOG_ERROR << "Problem database initialization failed: " << error->message;
    return 1;
  }
  auto problem_repository = std::get<std::unique_ptr<algorithm_trainer::SQLiteProblemRepository>>(
      std::move(problem_repository_result));
  algorithm_trainer::ProblemService problem_service{*problem_repository};
  algorithm_trainer::ProblemJudge judge{executor, problem_service};
  algorithm_trainer::SubmissionWorkerPool worker_pool{judge, *repository, 2};
  if (const auto error = worker_pool.start()) {
    LOG_ERROR << "Submission worker startup failed: " << *error;
    return 1;
  }
  algorithm_trainer::SubmissionService submission_service{*repository,
                                                          [&worker_pool] { worker_pool.notify(); }};
  auto auth_result = algorithm_trainer::AuthService::open(database_path);
  if (const auto *error = std::get_if<algorithm_trainer::AuthError>(&auth_result)) {
    LOG_ERROR << "Authentication initialization failed: " << error->message;
    return 1;
  }
  auto auth_service =
      std::get<std::unique_ptr<algorithm_trainer::AuthService>>(std::move(auth_result));
  auto admin_result = auth_service->ensure_admin("admin", "adminpassword");
  if (const auto *error = std::get_if<algorithm_trainer::AuthError>(&admin_result)) {
    LOG_ERROR << "Admin account initialization failed: " << error->message;
    return 1;
  }
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
                       [&submission_service, &auth_service, &problem_service](
                           const drogon::HttpRequestPtr &request, ResponseCallback &&callback) {
                         profile(request, std::move(callback), submission_service, *auth_service,
                                 problem_service);
                       },
                       {drogon::Get})
      .registerHandler(
          "/api/problems",
          [&problem_service](const drogon::HttpRequestPtr &request, ResponseCallback &&callback) {
            list_problems(request, std::move(callback), problem_service);
          },
          {drogon::Get})
      .registerHandler("/api/problems/{1}",
                       [&problem_service](const drogon::HttpRequestPtr &request,
                                          ResponseCallback &&callback, const std::string &slug) {
                         get_problem(request, std::move(callback), slug, problem_service);
                       },
                       {drogon::Get})
      .registerHandler("/api/problems/{1}/submissions",
                       [&submission_service, &auth_service, &problem_service](
                           const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
                           const std::string &problem_id) {
                         submission_history(request, std::move(callback), problem_id,
                                            submission_service, *auth_service, problem_service);
                       },
                       {drogon::Get})
      .registerHandler("/api/submissions/{1}",
                       [&submission_service](const drogon::HttpRequestPtr &request,
                                             ResponseCallback &&callback, const std::string &id) {
                         get_submission(request, std::move(callback), id, submission_service);
                       },
                       {drogon::Get})
      .registerHandler("/api/submissions",
                       [&submission_service, &auth_service, &problem_service](
                           const drogon::HttpRequestPtr &request, ResponseCallback &&callback) {
                         submit(request, std::move(callback), submission_service, *auth_service,
                                problem_service);
                       },
                       {drogon::Post})
      .registerHandler("/api/admin/problems",
                       [&auth_service, &problem_repository](const drogon::HttpRequestPtr &request,
                                                            ResponseCallback &&callback) {
                         admin_list_problems(request, std::move(callback), *auth_service,
                                             *problem_repository);
                       },
                       {drogon::Get})
      .registerHandler("/api/admin/problems",
                       [&auth_service, &problem_repository, &repository](
                           const drogon::HttpRequestPtr &request, ResponseCallback &&callback) {
                         admin_write_problem(request, std::move(callback), std::nullopt,
                                             *auth_service, *problem_repository, *repository);
                       },
                       {drogon::Post})
      .registerHandler(
          "/api/admin/problems/{1}",
          [&auth_service, &problem_repository](const drogon::HttpRequestPtr &request,
                                               ResponseCallback &&callback, const std::string &id) {
            admin_get_problem(request, std::move(callback), id, *auth_service, *problem_repository);
          },
          {drogon::Get})
      .registerHandler("/api/admin/problems/{1}",
                       [&auth_service, &problem_repository,
                        &repository](const drogon::HttpRequestPtr &request,
                                     ResponseCallback &&callback, const std::string &id) {
                         admin_write_problem(request, std::move(callback), id, *auth_service,
                                             *problem_repository, *repository);
                       },
                       {drogon::Put})
      .registerHandler("/api/admin/problems/{1}",
                       [&auth_service, &problem_repository,
                        &repository](const drogon::HttpRequestPtr &request,
                                     ResponseCallback &&callback, const std::string &id) {
                         admin_delete_problem(request, std::move(callback), id, *auth_service,
                                              *problem_repository, *repository);
                       },
                       {drogon::Delete})
      .registerHandler("/api/admin/problems/{1}/tests",
                       [&auth_service, &problem_repository,
                        &repository](const drogon::HttpRequestPtr &request,
                                     ResponseCallback &&callback, const std::string &id) {
                         admin_write_test(request, std::move(callback), id, std::nullopt,
                                          *auth_service, *problem_repository, *repository);
                       },
                       {drogon::Post})
      .registerHandler("/api/admin/problems/{1}/tests/{2}",
                       [&auth_service, &problem_repository, &repository](
                           const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
                           const std::string &id, const std::string &test_id) {
                         admin_write_test(request, std::move(callback), id, test_id, *auth_service,
                                          *problem_repository, *repository);
                       },
                       {drogon::Put})
      .registerHandler("/api/admin/problems/{1}/tests/{2}",
                       [&auth_service, &problem_repository, &repository](
                           const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
                           const std::string &id, const std::string &test_id) {
                         admin_delete_test(request, std::move(callback), id, test_id, *auth_service,
                                           *problem_repository, *repository);
                       },
                       {drogon::Delete})
      .registerHandler("/api/admin/submissions",
                       [&auth_service, &repository](const drogon::HttpRequestPtr &request,
                                                    ResponseCallback &&callback) {
                         admin_submissions(request, std::move(callback), std::nullopt,
                                           *auth_service, *repository);
                       },
                       {drogon::Get})
      .registerHandler(
          "/api/admin/submissions/{1}",
          [&auth_service, &repository](const drogon::HttpRequestPtr &request,
                                       ResponseCallback &&callback, const std::string &id) {
            admin_submissions(request, std::move(callback), id, *auth_service, *repository);
          },
          {drogon::Get})
      .registerHandler("/api/admin/submissions/{1}/retry",
                       [&auth_service, &repository,
                        &worker_pool](const drogon::HttpRequestPtr &request,
                                      ResponseCallback &&callback, const std::string &id) {
                         admin_retry_submission(request, std::move(callback), id, *auth_service,
                                                *repository,
                                                [&worker_pool] { worker_pool.notify(); });
                       },
                       {drogon::Post})
      .addListener("127.0.0.1", port)
      .run();
}
