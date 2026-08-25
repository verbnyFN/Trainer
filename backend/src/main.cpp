#include "algorithm-trainer/a-plus-b-judge.h"
#include "algorithm-trainer/nsjail-python-executor.h"
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

namespace {

using ResponseCallback = std::function<void(const drogon::HttpResponsePtr &)>;

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

void submit(const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
            algorithm_trainer::SubmissionService &submission_service) {
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

  auto result =
      submission_service.submit(std::get<algorithm_trainer::SubmissionRequest>(validation));
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
  algorithm_trainer::APlusBJudge judge{executor};
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

  drogon::app()
      .registerHandler(
          "/api/problems/{1}",
          [](const drogon::HttpRequestPtr &request, ResponseCallback &&callback,
             const std::string &slug) { get_problem(request, std::move(callback), slug); },
          {drogon::Get})
      .registerHandler("/api/submissions/{1}",
                       [&submission_service](const drogon::HttpRequestPtr &request,
                                             ResponseCallback &&callback, const std::string &id) {
                         get_submission(request, std::move(callback), id, submission_service);
                       },
                       {drogon::Get})
      .registerHandler("/api/submissions",
                       [&submission_service](const drogon::HttpRequestPtr &request,
                                             ResponseCallback &&callback) {
                         submit(request, std::move(callback), submission_service);
                       },
                       {drogon::Post})
      .addListener("127.0.0.1", port)
      .run();
}
