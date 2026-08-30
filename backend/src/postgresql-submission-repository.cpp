#include "algorithm-trainer/postgresql-submission-repository.h"
#include "postgresql-client.h"

#include <cctype>
#include <charconv>

namespace algorithm_trainer {
namespace {
constexpr std::string_view columns = "id,problem_id,language,source_code,status,verdict,created_at,"
                                     "completed_at,user_id,error_type,retry_of";
constexpr std::string_view qualified_columns =
    "submissions.id,submissions.problem_id,submissions.language,submissions.source_code,"
    "submissions.status,submissions.verdict,submissions.created_at,submissions.completed_at,"
    "submissions.user_id,submissions.error_type,submissions.retry_of";
std::int64_t integer(const std::string &value) {
  std::int64_t result{};
  std::from_chars(value.data(), value.data() + value.size(), result);
  return result;
}
std::optional<Verdict> verdict(std::string_view value) {
  if (value == "Accepted")
    return Verdict::accepted;
  if (value == "Wrong Answer")
    return Verdict::wrong_answer;
  if (value == "Runtime Error")
    return Verdict::runtime_error;
  if (value == "Time Limit Exceeded")
    return Verdict::time_limit_exceeded;
  return std::nullopt;
}
std::optional<SubmissionStatus> status(std::string_view value) {
  if (value == "queued")
    return SubmissionStatus::queued;
  if (value == "running")
    return SubmissionStatus::running;
  if (value == "completed")
    return SubmissionStatus::completed;
  if (value == "failed")
    return SubmissionStatus::failed;
  return std::nullopt;
}
std::variant<SubmissionRecord, RepositoryError> record(const postgresql::Result &result,
                                                       int row = 0) {
  auto parsed_status = status(result.value(row, 4));
  if (!parsed_status)
    return RepositoryError{"Database contains an unknown submission status"};
  std::optional<Verdict> parsed_verdict;
  if (!result.is_null(row, 5)) {
    parsed_verdict = verdict(result.value(row, 5));
    if (!parsed_verdict)
      return RepositoryError{"Database contains an unknown submission verdict"};
  }
  return SubmissionRecord{
      .id = SubmissionId{integer(result.value(row, 0))},
      .problem_id = result.value(row, 1),
      .language = result.value(row, 2),
      .source_code = result.value(row, 3),
      .status = *parsed_status,
      .verdict = parsed_verdict,
      .created_at = result.value(row, 6),
      .completed_at =
          result.is_null(row, 7) ? std::nullopt : std::optional<std::string>{result.value(row, 7)},
      .user_id = result.is_null(row, 8)
                     ? std::nullopt
                     : std::optional<std::int64_t>{integer(result.value(row, 8))},
      .error_type =
          result.is_null(row, 9) ? std::nullopt : std::optional<std::string>{result.value(row, 9)},
      .retry_of = result.is_null(row, 10)
                      ? std::nullopt
                      : std::optional<SubmissionId>{SubmissionId{integer(result.value(row, 10))}}};
}
RepositoryError failure(std::string_view operation, const postgresql::Result &result) {
  return RepositoryError{std::string{operation} + ": " + result.error()};
}
SubmissionHistoryResult records(const postgresql::Result &result) {
  if (!result.tuples_ok())
    return failure("Could not retrieve submissions", result);
  std::vector<SubmissionRecord> values;
  for (int row = 0; row < result.rows(); ++row) {
    auto value = record(result, row);
    if (auto *error = std::get_if<RepositoryError>(&value))
      return *error;
    values.push_back(std::get<SubmissionRecord>(std::move(value)));
  }
  return values;
}
} // namespace
struct PostgreSQLSubmissionRepository::Implementation {
  postgresql::Connection database;
  PostgreSQLRepositoryLimits limits;
  Implementation(const std::string &info, PostgreSQLRepositoryLimits configured)
      : database{info}, limits{configured} {}
};
PostgreSQLSubmissionRepository::PostgreSQLSubmissionRepository(
    std::unique_ptr<Implementation> value)
    : implementation_{std::move(value)} {}
PostgreSQLSubmissionRepository::~PostgreSQLSubmissionRepository() = default;
PostgreSQLSubmissionRepository::OpenResult
PostgreSQLSubmissionRepository::open(const std::string &info, PostgreSQLRepositoryLimits limits) {
  auto impl = std::make_unique<Implementation>(info, limits);
  if (!impl->database.valid())
    return RepositoryError{"Could not connect to PostgreSQL: " + impl->database.error()};
  auto probe = impl->database.execute("SELECT 1 FROM schema_migrations WHERE version=1");
  if (!probe.tuples_ok() || probe.rows() != 1)
    return RepositoryError{
        "PostgreSQL schema is missing; apply migrations/postgresql/001-initial-schema.sql first"};
  return std::unique_ptr<PostgreSQLSubmissionRepository>{
      new PostgreSQLSubmissionRepository{std::move(impl)}};
}
StoreSubmissionResult PostgreSQLSubmissionRepository::create(const SubmissionRequest &request) {
  auto result = implementation_->database.execute_locked(
      784512903,
      "INSERT INTO "
      "submissions(problem_id,language,source_code,status,user_id) SELECT $1,$2,$3,'queued',$4 "
      "WHERE (SELECT count(*) FROM submissions WHERE status "
      "IN('queued','running'))<$5::bigint AND (SELECT count(*) FROM submissions WHERE status "
      "IN('queued','running') AND user_id IS NOT DISTINCT FROM $4::bigint)<$6::bigint RETURNING " +
          std::string{columns},
      {postgresql::text(request.problem_id), postgresql::text(request.language),
       postgresql::text(request.code),
       request.user_id ? postgresql::number(*request.user_id) : std::nullopt,
       postgresql::number(implementation_->limits.maximum_active_submissions),
       postgresql::number(implementation_->limits.maximum_active_submissions_per_user)});
  if (!result.tuples_ok())
    return failure("Could not create submission", result);
  if (result.rows() == 1)
    return record(result);
  auto count = implementation_->database.execute(
      "SELECT count(*) FROM submissions WHERE status IN('queued','running') AND user_id IS NOT "
      "DISTINCT FROM $1::bigint",
      {request.user_id ? postgresql::number(*request.user_id) : std::nullopt});
  if (count.tuples_ok() && static_cast<std::size_t>(integer(count.value(0, 0))) >=
                               implementation_->limits.maximum_active_submissions_per_user)
    return RepositoryError{"Too many active submissions for this user",
                           RepositoryErrorCode::user_submission_limit};
  return RepositoryError{"Submission queue is full", RepositoryErrorCode::queue_full};
}
StoreSubmissionResult
PostgreSQLSubmissionRepository::complete(SubmissionId id, Verdict value,
                                         std::optional<std::string> error_type) {
  auto result = implementation_->database.execute(
      "UPDATE submissions SET "
      "status='completed',verdict=$1,error_type=$2,completed_at=clock_timestamp() WHERE id=$3 AND "
      "status='running' RETURNING " +
          std::string{columns},
      {postgresql::text(verdict_name(value)), error_type, postgresql::number(id.value)});
  if (!result.tuples_ok())
    return failure("Could not complete submission", result);
  if (result.rows() != 1)
    return RepositoryError{"Submission is missing or is not running"};
  return record(result);
}
StoreSubmissionResult PostgreSQLSubmissionRepository::fail(SubmissionId id,
                                                           std::optional<std::string> error_type) {
  auto result = implementation_->database.execute(
      "UPDATE submissions SET status='failed',error_type=$1,completed_at=clock_timestamp() WHERE "
      "id=$2 AND status='running' RETURNING " +
          std::string{columns},
      {error_type, postgresql::number(id.value)});
  if (!result.tuples_ok())
    return failure("Could not fail submission", result);
  if (result.rows() != 1)
    return RepositoryError{"Submission is missing or is not running"};
  return record(result);
}
ClaimSubmissionResult PostgreSQLSubmissionRepository::claim_next() {
  auto result = implementation_->database.execute_locked(
      784512904,
      "WITH candidate AS (SELECT "
      "queued.id FROM submissions queued WHERE queued.status='queued' AND (SELECT "
      "count(*) FROM submissions running WHERE running.status='running' AND running.user_id IS NOT "
      "DISTINCT FROM queued.user_id)<$1::bigint ORDER BY queued.id FOR UPDATE OF queued SKIP "
      "LOCKED LIMIT 1) UPDATE submissions SET status='running' FROM candidate WHERE "
      "submissions.id=candidate.id RETURNING " +
          std::string{qualified_columns},
      {postgresql::number(implementation_->limits.maximum_running_submissions_per_user)});
  if (!result.tuples_ok())
    return failure("Could not claim queued submission", result);
  if (result.rows() == 0)
    return std::optional<SubmissionRecord>{};
  auto value = record(result);
  if (auto *error = std::get_if<RepositoryError>(&value))
    return *error;
  return std::optional<SubmissionRecord>{std::get<SubmissionRecord>(std::move(value))};
}
RecoverSubmissionsResult PostgreSQLSubmissionRepository::recover_running() {
  auto result = implementation_->database.execute(
      "UPDATE submissions SET status='queued' WHERE status='running' RETURNING id");
  if (!result.tuples_ok())
    return failure("Could not recover running submissions", result);
  return static_cast<std::size_t>(result.rows());
}
FindSubmissionResult PostgreSQLSubmissionRepository::find(SubmissionId id) {
  auto result = implementation_->database.execute("SELECT " + std::string{columns} +
                                                      " FROM submissions WHERE id=$1",
                                                  {postgresql::number(id.value)});
  if (!result.tuples_ok())
    return failure("Could not retrieve submission", result);
  if (result.rows() == 0)
    return std::optional<SubmissionRecord>{};
  auto value = record(result);
  if (auto *error = std::get_if<RepositoryError>(&value))
    return *error;
  return std::optional<SubmissionRecord>{std::get<SubmissionRecord>(std::move(value))};
}
SubmissionHistoryResult PostgreSQLSubmissionRepository::history(std::int64_t user_id,
                                                                const std::string &problem_id) {
  return records(implementation_->database.execute(
      "SELECT " + std::string{columns} +
          " FROM submissions WHERE user_id=$1 AND problem_id=$2 ORDER BY created_at DESC,id DESC",
      {postgresql::number(user_id), postgresql::text(problem_id)}));
}
SubmissionHistoryResult PostgreSQLSubmissionRepository::list_all() {
  return records(implementation_->database.execute(
      "SELECT " + std::string{columns} + " FROM submissions ORDER BY created_at DESC,id DESC"));
}
SubmissionHistoryResult
PostgreSQLSubmissionRepository::list_filtered(const SubmissionAdminFilter &filter) {
  std::string sql = "SELECT " + std::string{columns} + " FROM submissions WHERE true";
  std::vector<std::optional<std::string>> params;
  auto add = [&](std::string clause, std::optional<std::string> value) {
    if (value) {
      params.push_back(std::move(value));
      sql += clause + "$" + std::to_string(params.size());
    }
  };
  if (filter.status) {
    auto value = std::string{submission_status_name(*filter.status)};
    value[0] = static_cast<char>(std::tolower(value[0]));
    add(" AND status=", value);
  }
  add(" AND error_type=", filter.error_type);
  add(" AND language=", filter.language);
  add(" AND problem_id=", filter.problem_id);
  if (filter.user_id)
    add(" AND user_id=", std::to_string(*filter.user_id));
  add(" AND created_at>=", filter.created_from);
  add(" AND created_at<=", filter.created_to);
  sql += " ORDER BY created_at DESC,id DESC LIMIT 500";
  return records(implementation_->database.execute(sql, params));
}
StoreSubmissionResult PostgreSQLSubmissionRepository::retry(SubmissionId id, std::int64_t admin) {
  auto result = implementation_->database.execute_locked(
      784512903,
      "WITH retried AS (INSERT INTO "
      "submissions(problem_id,language,source_code,status,user_id,retry_of) SELECT "
      "problem_id,language,source_code,'queued',user_id,id FROM submissions "
      "original WHERE original.id=$1 AND (status='failed' OR verdict='Runtime Error') AND "
      "NOT EXISTS(SELECT 1 FROM submissions WHERE retry_of=$1) AND (SELECT count(*) FROM "
      "submissions WHERE status IN('queued','running'))<$2::bigint AND (SELECT count(*) FROM "
      "submissions WHERE status IN('queued','running') AND user_id IS NOT DISTINCT FROM "
      "original.user_id)<$3::bigint RETURNING " +
          std::string{columns} +
          ") , audited AS (INSERT INTO "
          "admin_audit_log(admin_user_id,action,entity_type,entity_id,details_json) SELECT "
          "$4,'submission.retry','submission',$1,jsonb_build_object('retrySubmissionId',id) FROM "
          "retried) SELECT * FROM retried",
      {postgresql::number(id.value),
       postgresql::number(implementation_->limits.maximum_active_submissions),
       postgresql::number(implementation_->limits.maximum_active_submissions_per_user),
       postgresql::number(admin)});
  if (!result.tuples_ok())
    return failure("Could not retry submission", result);
  if (result.rows() != 1)
    return RepositoryError{"Submission is not retryable or was already retried"};
  return record(result);
}
std::variant<std::monostate, RepositoryError>
PostgreSQLSubmissionRepository::audit(std::int64_t admin, const std::string &action,
                                      const std::string &type, const std::string &id,
                                      const std::string &details) {
  auto result = implementation_->database.execute(
      "INSERT INTO admin_audit_log(admin_user_id,action,entity_type,entity_id,details_json) "
      "VALUES($1,$2,$3,$4,$5::jsonb)",
      {postgresql::number(admin), postgresql::text(action), postgresql::text(type),
       postgresql::text(id), postgresql::text(details)});
  if (!result.command_ok())
    return failure("Could not write admin audit record", result);
  return std::monostate{};
}
UserProgressResult PostgreSQLSubmissionRepository::progress(std::int64_t user) {
  auto result = implementation_->database.execute(
      "SELECT count(*),count(*) FILTER(WHERE verdict='Accepted'),(SELECT problem_id FROM "
      "submissions WHERE user_id=$1 ORDER BY created_at DESC,id DESC LIMIT 1) FROM submissions "
      "WHERE user_id=$1",
      {postgresql::number(user)});
  if (!result.tuples_ok())
    return failure("Could not retrieve user activity", result);
  UserProgress value{.total_submissions = integer(result.value(0, 0)),
                     .accepted_submissions = integer(result.value(0, 1)),
                     .most_recent_submission_problem_id =
                         result.is_null(0, 2) ? std::nullopt
                                              : std::optional<std::string>{result.value(0, 2)}};
  auto streak = implementation_->database.execute(
      "WITH days AS(SELECT DISTINCT completed_at::date AS completion_day FROM submissions WHERE "
      "user_id=$1 AND verdict='Accepted'),numbered AS(SELECT completion_day,max(completion_day) "
      "OVER()-completion_day AS distance,row_number() OVER(ORDER BY completion_day DESC)-1 AS "
      "position FROM days) SELECT CASE WHEN coalesce(max(completion_day),DATE "
      "'-infinity')<current_date-1 THEN 0 ELSE count(*) FILTER(WHERE distance=position) END FROM "
      "numbered",
      {postgresql::number(user)});
  if (!streak.tuples_ok())
    return failure("Could not retrieve user streak", streak);
  value.current_streak_days = integer(streak.value(0, 0));
  auto completed = implementation_->database.execute(
      "SELECT problem_id,min(completed_at) FROM submissions WHERE user_id=$1 AND "
      "verdict='Accepted' GROUP BY problem_id ORDER BY min(completed_at) DESC",
      {postgresql::number(user)});
  if (!completed.tuples_ok())
    return failure("Could not retrieve completed problems", completed);
  for (int row = 0; row < completed.rows(); ++row)
    value.completed_problems.push_back(
        {.problem_id = completed.value(row, 0), .completed_at = completed.value(row, 1)});
  return value;
}
} // namespace algorithm_trainer
