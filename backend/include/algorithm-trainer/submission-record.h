#pragma once

#include "algorithm-trainer/judge.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace algorithm_trainer {

struct SubmissionId {
  std::int64_t value{};
  auto operator<=>(const SubmissionId &) const = default;
};

enum class SubmissionStatus : std::uint8_t { pending, completed, failed };

[[nodiscard]] std::string_view submission_status_name(SubmissionStatus status);

struct SubmissionRecord {
  SubmissionId id;
  std::string problem_id;
  std::string language;
  std::string source_code;
  SubmissionStatus status{SubmissionStatus::pending};
  std::optional<Verdict> verdict;
  std::string created_at;
  std::optional<std::string> completed_at;
  std::optional<std::int64_t> user_id;
};

} // namespace algorithm_trainer
