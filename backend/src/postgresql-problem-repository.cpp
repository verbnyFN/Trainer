#include "algorithm-trainer/postgresql-problem-repository.h"

#include "algorithm-trainer/problem-seed.h"
#include "postgresql-client.h"

#include <json/json.h>

#include <charconv>
#include <sstream>

namespace algorithm_trainer {
namespace {
using postgresql::boolean;
using postgresql::number;
using postgresql::text;

std::string json_text(const Json::Value &value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, value);
}
Json::Value strings_json(const std::vector<std::string> &values) {
  Json::Value json{Json::arrayValue};
  for (const auto &value : values)
    json.append(value);
  return json;
}
Json::Value examples_json(const std::vector<ProblemExample> &examples) {
  Json::Value json{Json::arrayValue};
  for (const auto &example : examples) {
    Json::Value value;
    value["input"] = example.input;
    value["output"] = example.output;
    json.append(std::move(value));
  }
  return json;
}
std::optional<Json::Value> parse_json(const std::string &value) {
  Json::CharReaderBuilder builder;
  Json::Value json;
  std::string errors;
  std::istringstream input{value};
  if (!Json::parseFromStream(builder, input, &json, &errors))
    return std::nullopt;
  return json;
}
std::int64_t integer(const std::string &value) {
  std::int64_t result{};
  std::from_chars(value.data(), value.data() + value.size(), result);
  return result;
}
std::optional<ProblemDifficulty> difficulty(std::string_view value) {
  if (value == "Easy")
    return ProblemDifficulty::easy;
  if (value == "Medium")
    return ProblemDifficulty::medium;
  if (value == "Hard")
    return ProblemDifficulty::hard;
  return std::nullopt;
}
constexpr std::string_view columns =
    "id, title, description, input_format, output_format, difficulty, "
    "tags_json::text, languages_json::text, examples_json::text, enabled, revision";

std::variant<Problem, ProblemRepositoryError> read_problem(postgresql::Connection &database,
                                                           const postgresql::Result &row, int index,
                                                           bool include_tests,
                                                           bool include_disabled = false) {
  const auto parsed_difficulty = difficulty(row.value(index, 5));
  const auto tags = parse_json(row.value(index, 6));
  const auto languages = parse_json(row.value(index, 7));
  const auto examples = parse_json(row.value(index, 8));
  if (!parsed_difficulty || !tags || !tags->isArray() || !languages || !languages->isArray() ||
      !examples || !examples->isArray())
    return ProblemRepositoryError{"Database contains invalid problem metadata"};
  Problem problem{.id = row.value(index, 0),
                  .title = row.value(index, 1),
                  .description = row.value(index, 2),
                  .input_format = row.value(index, 3),
                  .output_format = row.value(index, 4),
                  .difficulty = *parsed_difficulty,
                  .enabled = row.value(index, 9) == "t",
                  .revision = integer(row.value(index, 10))};
  for (const auto &tag : *tags)
    problem.tags.push_back(tag.asString());
  for (const auto &language : *languages)
    problem.languages.push_back(language.asString());
  for (const auto &example : *examples) {
    if (!example.isObject() || !example["input"].isString() || !example["output"].isString())
      return ProblemRepositoryError{"Database contains invalid problem examples"};
    problem.examples.push_back({example["input"].asString(), example["output"].asString()});
  }
  if (!include_tests)
    return problem;
  auto tests = database.execute(
      include_disabled
          ? "SELECT input, expected_output, id, position, enabled, revision FROM problem_tests "
            "WHERE problem_id = $1 ORDER BY position, id"
          : "SELECT input, expected_output, id, position, enabled, revision FROM problem_tests "
            "WHERE problem_id = $1 AND enabled ORDER BY position, id",
      {text(problem.id)});
  if (!tests.tuples_ok())
    return ProblemRepositoryError{"Could not retrieve hidden tests"};
  for (int test = 0; test < tests.rows(); ++test)
    problem.hidden_tests.push_back({.input = tests.value(test, 0),
                                    .expected_output = tests.value(test, 1),
                                    .id = integer(tests.value(test, 2)),
                                    .position = integer(tests.value(test, 3)),
                                    .enabled = tests.value(test, 4) == "t",
                                    .revision = integer(tests.value(test, 5))});
  return problem;
}

ProblemListResult list(postgresql::Connection &database, std::string_view condition) {
  auto result = database.execute("SELECT " + std::string{columns} + " FROM problems " +
                                 std::string{condition} + " ORDER BY created_at, id");
  if (!result.tuples_ok())
    return ProblemRepositoryError{"Could not retrieve problem list"};
  std::vector<Problem> problems;
  for (int row = 0; row < result.rows(); ++row) {
    auto problem = read_problem(database, result, row, false);
    if (auto *error = std::get_if<ProblemRepositoryError>(&problem))
      return *error;
    problems.push_back(std::get<Problem>(std::move(problem)));
  }
  return problems;
}

ProblemFindResult find(postgresql::Connection &database, const std::string &id,
                       std::string_view condition, bool tests, bool disabled_tests = false) {
  auto result = database.execute("SELECT " + std::string{columns} +
                                     " FROM problems WHERE id = $1 " + std::string{condition},
                                 {text(id)});
  if (!result.tuples_ok())
    return ProblemRepositoryError{"Could not retrieve problem"};
  if (result.rows() == 0)
    return std::optional<Problem>{};
  auto problem = read_problem(database, result, 0, tests, disabled_tests);
  if (auto *error = std::get_if<ProblemRepositoryError>(&problem))
    return *error;
  return std::optional<Problem>{std::get<Problem>(std::move(problem))};
}
} // namespace

struct PostgreSQLProblemRepository::Implementation {
  postgresql::Connection database;
  explicit Implementation(const std::string &info) : database{info} {}
};
PostgreSQLProblemRepository::PostgreSQLProblemRepository(std::unique_ptr<Implementation> value)
    : implementation_{std::move(value)} {}
PostgreSQLProblemRepository::~PostgreSQLProblemRepository() = default;
PostgreSQLProblemRepository::OpenResult PostgreSQLProblemRepository::open(const std::string &info) {
  auto impl = std::make_unique<Implementation>(info);
  if (!impl->database.valid())
    return ProblemRepositoryError{"Could not connect to PostgreSQL: " + impl->database.error()};
  if (!impl->database.execute("BEGIN").command_ok())
    return ProblemRepositoryError{"Could not begin PostgreSQL problem seeding"};
  if (!impl->database.execute("SELECT pg_advisory_xact_lock(784512902)").tuples_ok()) {
    static_cast<void>(impl->database.execute("ROLLBACK"));
    return ProblemRepositoryError{"Could not lock PostgreSQL problem seeding"};
  }
  for (const auto &problem : default_problems()) {
    auto inserted = impl->database.execute(
        "INSERT INTO "
        "problems(id,title,description,input_format,output_format,difficulty,tags_json,languages_"
        "json,examples_json) "
        "VALUES($1,$2,$3,$4,$5,$6,$7::jsonb,$8::jsonb,$9::jsonb) ON CONFLICT(id) DO NOTHING "
        "RETURNING id",
        {text(problem.id), text(problem.title), text(problem.description),
         text(problem.input_format), text(problem.output_format),
         text(difficulty_name(problem.difficulty)), text(json_text(strings_json(problem.tags))),
         text(json_text(strings_json(problem.languages))),
         text(json_text(examples_json(problem.examples)))});
    if (!inserted.tuples_ok()) {
      static_cast<void>(impl->database.execute("ROLLBACK"));
      return ProblemRepositoryError{"Could not seed PostgreSQL problems: " + inserted.error()};
    }
    if (inserted.rows() == 0)
      continue;
    for (std::size_t index = 0; index < problem.hidden_tests.size(); ++index) {
      auto test = impl->database.execute(
          "INSERT INTO problem_tests(problem_id,input,expected_output,position) "
          "VALUES($1,$2,$3,$4)",
          {text(problem.id), text(problem.hidden_tests[index].input),
           text(problem.hidden_tests[index].expected_output), number(index)});
      if (!test.command_ok()) {
        static_cast<void>(impl->database.execute("ROLLBACK"));
        return ProblemRepositoryError{"Could not seed PostgreSQL hidden tests: " + test.error()};
      }
    }
  }
  if (!impl->database.execute("COMMIT").command_ok()) {
    static_cast<void>(impl->database.execute("ROLLBACK"));
    return ProblemRepositoryError{"Could not commit PostgreSQL problem seeding"};
  }
  return std::unique_ptr<PostgreSQLProblemRepository>{
      new PostgreSQLProblemRepository{std::move(impl)}};
}
ProblemListResult PostgreSQLProblemRepository::list_enabled() {
  return list(implementation_->database, "WHERE enabled");
}
ProblemListResult PostgreSQLProblemRepository::list_all() {
  return list(implementation_->database, "");
}
ProblemFindResult PostgreSQLProblemRepository::find_enabled(const std::string &id) {
  return find(implementation_->database, id, "AND enabled", false);
}
ProblemFindResult PostgreSQLProblemRepository::find_for_judging(const std::string &id) {
  return find(implementation_->database, id, "", true);
}
ProblemFindResult PostgreSQLProblemRepository::find_any(const std::string &id) {
  return find(implementation_->database, id, "", true, true);
}

ProblemWriteResult PostgreSQLProblemRepository::create(const Problem &problem) {
  auto result = implementation_->database.execute(
      "INSERT INTO "
      "problems(id,title,description,input_format,output_format,difficulty,tags_json,languages_"
      "json,examples_json,enabled) "
      "VALUES($1,$2,$3,$4,$5,$6,$7::jsonb,$8::jsonb,$9::jsonb,$10) RETURNING revision",
      {text(problem.id), text(problem.title), text(problem.description), text(problem.input_format),
       text(problem.output_format), text(difficulty_name(problem.difficulty)),
       text(json_text(strings_json(problem.tags))),
       text(json_text(strings_json(problem.languages))),
       text(json_text(examples_json(problem.examples))), boolean(problem.enabled)});
  if (!result.tuples_ok() || result.rows() != 1)
    return ProblemRepositoryError{"Problem could not be created"};
  auto created = problem;
  created.revision = integer(result.value(0, 0));
  return created;
}
ProblemWriteResult PostgreSQLProblemRepository::update(const Problem &problem) {
  auto result = implementation_->database.execute(
      "UPDATE problems SET "
      "title=$1,description=$2,input_format=$3,output_format=$4,difficulty=$5,tags_json=$6::jsonb,"
      "languages_json=$7::jsonb,examples_json=$8::jsonb,enabled=$9,revision=revision+1,updated_at="
      "clock_timestamp() WHERE id=$10 AND revision=$11 RETURNING revision",
      {text(problem.title), text(problem.description), text(problem.input_format),
       text(problem.output_format), text(difficulty_name(problem.difficulty)),
       text(json_text(strings_json(problem.tags))),
       text(json_text(strings_json(problem.languages))),
       text(json_text(examples_json(problem.examples))), boolean(problem.enabled), text(problem.id),
       number(problem.revision)});
  if (!result.tuples_ok())
    return ProblemRepositoryError{"Problem could not be updated"};
  if (result.rows() == 0)
    return ProblemRepositoryError{"Problem was changed by another administrator"};
  auto updated = problem;
  updated.revision = integer(result.value(0, 0));
  return updated;
}
ProblemDeleteResult PostgreSQLProblemRepository::remove(const std::string &id) {
  auto result = implementation_->database.execute("DELETE FROM problems WHERE id=$1 RETURNING id",
                                                  {text(id)});
  if (!result.tuples_ok())
    return ProblemRepositoryError{"Problem could not be deleted"};
  return result.rows() != 0;
}
ProblemTestWriteResult PostgreSQLProblemRepository::create_test(const std::string &problem_id,
                                                                const ProblemTestCase &test) {
  auto result = implementation_->database.execute(
      "INSERT INTO problem_tests(problem_id,input,expected_output,position,enabled) "
      "VALUES($1,$2,$3,$4,$5) RETURNING id,revision",
      {text(problem_id), text(test.input), text(test.expected_output), number(test.position),
       boolean(test.enabled)});
  if (!result.tuples_ok() || result.rows() != 1)
    return ProblemRepositoryError{"Hidden test could not be created"};
  auto created = test;
  created.id = integer(result.value(0, 0));
  created.revision = integer(result.value(0, 1));
  return created;
}
ProblemTestWriteResult PostgreSQLProblemRepository::update_test(const std::string &problem_id,
                                                                const ProblemTestCase &test) {
  auto result = implementation_->database.execute(
      "UPDATE problem_tests SET "
      "input=$1,expected_output=$2,position=$3,enabled=$4,revision=revision+1 WHERE id=$5 AND "
      "problem_id=$6 AND revision=$7 RETURNING revision",
      {text(test.input), text(test.expected_output), number(test.position), boolean(test.enabled),
       number(test.id), text(problem_id), number(test.revision)});
  if (!result.tuples_ok())
    return ProblemRepositoryError{"Hidden test could not be updated"};
  if (result.rows() == 0)
    return ProblemRepositoryError{"Hidden test was changed by another administrator"};
  auto updated = test;
  updated.revision = integer(result.value(0, 0));
  return updated;
}
ProblemDeleteResult PostgreSQLProblemRepository::remove_test(const std::string &problem_id,
                                                             std::int64_t test_id) {
  auto result = implementation_->database.execute(
      "DELETE FROM problem_tests WHERE id=$1 AND problem_id=$2 RETURNING id",
      {number(test_id), text(problem_id)});
  if (!result.tuples_ok())
    return ProblemRepositoryError{"Hidden test could not be deleted"};
  return result.rows() != 0;
}
} // namespace algorithm_trainer
