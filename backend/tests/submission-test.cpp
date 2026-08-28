#include "algorithm-trainer/submission.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <variant>

namespace {

Json::Value valid_request() {
  Json::Value json;
  json["problemId"] = "a-plus-b";
  json["language"] = "python";
  json["code"] = "print(sum(map(int, input().split())))";
  return json;
}

std::string validation_error(const Json::Value &json) {
  const auto result = algorithm_trainer::validate_submission(json);
  REQUIRE(std::holds_alternative<algorithm_trainer::ValidationError>(result));
  return std::get<algorithm_trainer::ValidationError>(result).message;
}

} // namespace

TEST_CASE("a valid Python submission is accepted", "[submission]") {
  const auto result = algorithm_trainer::validate_submission(valid_request());

  REQUIRE(std::holds_alternative<algorithm_trainer::SubmissionRequest>(result));
  const auto &submission = std::get<algorithm_trainer::SubmissionRequest>(result);
  CHECK(submission.problem_id == "a-plus-b");
  CHECK(submission.language == "python");
  CHECK_FALSE(submission.code.empty());
}

TEST_CASE("a valid C++ submission is accepted", "[submission]") {
  auto json = valid_request();
  json["language"] = "cpp";
  json["code"] = "#include <iostream>\nint main() { std::cout << 3 << '\\n'; }";

  const auto result = algorithm_trainer::validate_submission(json);

  REQUIRE(std::holds_alternative<algorithm_trainer::SubmissionRequest>(result));
  CHECK(std::get<algorithm_trainer::SubmissionRequest>(result).language == "cpp");
}

TEST_CASE("submission fields are required strings", "[submission]") {
  auto json = valid_request();
  json.removeMember("code");
  CHECK(validation_error(json) == "code must be a string");

  json = valid_request();
  json["language"] = 1;
  CHECK(validation_error(json) == "language must be a string");
}

TEST_CASE("problem identifiers and configured languages are structurally validated",
          "[submission]") {
  auto merge_sort = valid_request();
  merge_sort["problemId"] = "merge-sort";
  CHECK(std::holds_alternative<algorithm_trainer::SubmissionRequest>(
      algorithm_trainer::validate_submission(merge_sort)));

  auto json = valid_request();
  json["problemId"] = "other-problem";
  CHECK(std::holds_alternative<algorithm_trainer::SubmissionRequest>(
      algorithm_trainer::validate_submission(json)));

  json = valid_request();
  json["language"] = "rust";
  CHECK(validation_error(json) == "Unsupported language");
}

TEST_CASE("submission code must be non-empty and bounded", "[submission]") {
  auto json = valid_request();
  json["code"] = "";
  CHECK(validation_error(json) == "code must not be empty");

  json["code"] = std::string(64 * 1024 + 1, 'x');
  CHECK(validation_error(json) == "code is too large");
}
