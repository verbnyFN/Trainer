#include "algorithm-trainer/submission-worker.h"

#include <exception>
#include <utility>
#include <variant>

namespace algorithm_trainer {

SubmissionWorkerPool::SubmissionWorkerPool(Judge &judge, SubmissionRepository &repository,
                                           std::size_t worker_count,
                                           std::chrono::milliseconds idle_poll_interval)
    : judge_{judge}, repository_{repository}, worker_count_{worker_count},
      idle_poll_interval_{idle_poll_interval} {}

SubmissionWorkerPool::~SubmissionWorkerPool() { stop(); }

std::optional<std::string> SubmissionWorkerPool::start() {
  if (started_) {
    return std::nullopt;
  }
  auto recovered = repository_.recover_running();
  if (const auto *error = std::get_if<RepositoryError>(&recovered)) {
    return error->message;
  }
  stopping_ = false;
  started_ = true;
  workers_.reserve(worker_count_);
  for (std::size_t index = 0; index < worker_count_; ++index) {
    workers_.emplace_back(&SubmissionWorkerPool::worker_loop, this);
  }
  return std::nullopt;
}

void SubmissionWorkerPool::stop() {
  {
    std::lock_guard lock{mutex_};
    if (!started_) {
      return;
    }
    stopping_ = true;
  }
  condition_.notify_all();
  for (auto &worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
  started_ = false;
}

void SubmissionWorkerPool::notify() { condition_.notify_one(); }

SubmissionWorkerPool::WorkResult SubmissionWorkerPool::process_one() {
  auto claimed = repository_.claim_next();
  if (std::holds_alternative<RepositoryError>(claimed)) {
    return WorkResult::repository_error;
  }
  auto submission = std::get<std::optional<SubmissionRecord>>(std::move(claimed));
  if (!submission) {
    return WorkResult::idle;
  }

  const auto complete = [this, &submission](Verdict verdict,
                                            std::optional<std::string> error_type = std::nullopt) {
    auto result = repository_.complete(submission->id, verdict, std::move(error_type));
    if (std::holds_alternative<RepositoryError>(result)) {
      static_cast<void>(repository_.fail(submission->id, "Result Persistence Error"));
    }
  };

  try {
    const auto judged = judge_.run({
        .problem_id = submission->problem_id,
        .language = submission->language,
        .source_code = submission->source_code,
    });
    if (std::holds_alternative<JudgeError>(judged)) {
      static_cast<void>(repository_.fail(submission->id, "Sandbox Error"));
      return WorkResult::processed;
    }
    if (const auto *runtime_error = std::get_if<RuntimeError>(&judged)) {
      complete(Verdict::runtime_error, runtime_error->type);
      return WorkResult::processed;
    }
    complete(std::get<Verdict>(judged));
  } catch (const std::exception &) {
    static_cast<void>(repository_.fail(submission->id, "Worker Error"));
  } catch (...) {
    static_cast<void>(repository_.fail(submission->id, "Worker Error"));
  }
  return WorkResult::processed;
}

void SubmissionWorkerPool::worker_loop() {
  while (true) {
    {
      std::lock_guard lock{mutex_};
      if (stopping_) {
        return;
      }
    }
    const auto result = process_one();
    if (result == WorkResult::processed) {
      continue;
    }
    std::unique_lock lock{mutex_};
    condition_.wait_for(lock, idle_poll_interval_);
    if (stopping_) {
      return;
    }
  }
}

} // namespace algorithm_trainer
