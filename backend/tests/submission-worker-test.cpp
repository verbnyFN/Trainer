#include "algorithm-trainer/judge.h"
#include "algorithm-trainer/sqlite-submission-repository.h"
#include "algorithm-trainer/submission-worker.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    std::array<char, 64> path_template{};
    constexpr std::string_view pattern{"/tmp/algorithm-trainer-worker-XXXXXX"};
    std::ranges::copy(pattern, path_template.begin());
    const auto *created = ::mkdtemp(path_template.data());
    REQUIRE(created != nullptr);
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] std::filesystem::path database() const { return path_ / "test.sqlite3"; }

private:
  std::filesystem::path path_;
};

std::unique_ptr<algorithm_trainer::SQLiteSubmissionRepository>
open_repository(const std::filesystem::path &path,
                algorithm_trainer::SubmissionQueueLimits limits = {}) {
  auto result = algorithm_trainer::SQLiteSubmissionRepository::open(path, limits);
  REQUIRE(std::holds_alternative<std::unique_ptr<algorithm_trainer::SQLiteSubmissionRepository>>(
      result));
  return std::get<std::unique_ptr<algorithm_trainer::SQLiteSubmissionRepository>>(
      std::move(result));
}

algorithm_trainer::SubmissionRecord create(algorithm_trainer::SubmissionRepository &repository,
                                           std::string source) {
  auto result = repository.create({
      .problem_id = "a-plus-b",
      .language = "python",
      .code = std::move(source),
  });
  REQUIRE(std::holds_alternative<algorithm_trainer::SubmissionRecord>(result));
  return std::get<algorithm_trainer::SubmissionRecord>(std::move(result));
}

algorithm_trainer::SubmissionRecord
wait_for_terminal(algorithm_trainer::SubmissionRepository &repository,
                  algorithm_trainer::SubmissionId id) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < deadline) {
    auto result = repository.find(id);
    REQUIRE(std::holds_alternative<std::optional<algorithm_trainer::SubmissionRecord>>(result));
    const auto &record = std::get<std::optional<algorithm_trainer::SubmissionRecord>>(result);
    REQUIRE(record.has_value());
    if (record->status == algorithm_trainer::SubmissionStatus::completed ||
        record->status == algorithm_trainer::SubmissionStatus::failed) {
      return *record;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  FAIL("Submission did not reach a terminal state");
}

class CountingJudge final : public algorithm_trainer::Judge {
public:
  algorithm_trainer::JudgeResult run(const algorithm_trainer::JudgeRequest &request) override {
    {
      std::lock_guard lock{mutex_};
      ++counts_[request.source_code];
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    return algorithm_trainer::Verdict::accepted;
  }

  [[nodiscard]] std::size_t count(const std::string &source) const {
    std::lock_guard lock{mutex_};
    const auto found = counts_.find(source);
    return found == counts_.end() ? 0 : found->second;
  }

private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::size_t> counts_;
};

class FailingJudge final : public algorithm_trainer::Judge {
public:
  algorithm_trainer::JudgeResult run(const algorithm_trainer::JudgeRequest &) override {
    return algorithm_trainer::JudgeError{"sandbox unavailable"};
  }
};

} // namespace

TEST_CASE("Concurrent workers process each queued submission exactly once", "[worker][sqlite]") {
  TemporaryDirectory directory;
  auto repository =
      open_repository(directory.database(), {
                                                .maximum_active_submissions = 50,
                                                .maximum_active_submissions_per_user = 50,
                                                .maximum_running_submissions_per_user = 4,
                                            });
  std::vector<algorithm_trainer::SubmissionRecord> queued;
  for (int index = 0; index < 24; ++index) {
    queued.push_back(create(*repository, "submission-" + std::to_string(index)));
  }
  CountingJudge judge;
  algorithm_trainer::SubmissionWorkerPool workers{judge, *repository, 4,
                                                  std::chrono::milliseconds{10}};
  REQUIRE_FALSE(workers.start().has_value());
  workers.notify();

  for (const auto &submission : queued) {
    const auto completed = wait_for_terminal(*repository, submission.id);
    CHECK(completed.status == algorithm_trainer::SubmissionStatus::completed);
    CHECK(completed.verdict == algorithm_trainer::Verdict::accepted);
    CHECK(judge.count(submission.source_code) == 1);
  }
}

TEST_CASE("Queued and interrupted submissions survive repository restart", "[worker][sqlite]") {
  TemporaryDirectory directory;
  algorithm_trainer::SubmissionId running_id;
  algorithm_trainer::SubmissionId queued_id;
  {
    auto repository = open_repository(directory.database());
    running_id = create(*repository, "interrupted").id;
    queued_id = create(*repository, "still-queued").id;
    auto claimed = repository->claim_next();
    REQUIRE(std::holds_alternative<std::optional<algorithm_trainer::SubmissionRecord>>(claimed));
    REQUIRE(std::get<std::optional<algorithm_trainer::SubmissionRecord>>(claimed).has_value());
  }

  auto reopened = open_repository(directory.database());
  CountingJudge judge;
  algorithm_trainer::SubmissionWorkerPool worker{judge, *reopened, 1,
                                                 std::chrono::milliseconds{10}};
  REQUIRE_FALSE(worker.start().has_value());
  worker.notify();

  CHECK(wait_for_terminal(*reopened, running_id).verdict == algorithm_trainer::Verdict::accepted);
  CHECK(wait_for_terminal(*reopened, queued_id).verdict == algorithm_trainer::Verdict::accepted);
  CHECK(judge.count("interrupted") == 1);
  CHECK(judge.count("still-queued") == 1);
}

TEST_CASE("Judge infrastructure failures become terminal failed submissions", "[worker][sqlite]") {
  TemporaryDirectory directory;
  auto repository = open_repository(directory.database());
  const auto queued = create(*repository, "fails-cleanly");
  FailingJudge judge;
  algorithm_trainer::SubmissionWorkerPool worker{judge, *repository, 1,
                                                 std::chrono::milliseconds{10}};
  REQUIRE_FALSE(worker.start().has_value());
  worker.notify();

  const auto failed = wait_for_terminal(*repository, queued.id);
  CHECK(failed.status == algorithm_trainer::SubmissionStatus::failed);
  CHECK(failed.error_type == "Sandbox Error");
}
