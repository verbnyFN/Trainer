#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string_view>

namespace algorithm_trainer {

inline constexpr std::size_t maximum_problem_text_bytes{64 * 1024};
inline constexpr std::size_t maximum_hidden_test_bytes{1024 * 1024};

[[nodiscard]] inline bool valid_problem_slug(std::string_view slug) {
  return !slug.empty() && slug.size() <= 80 && slug.front() != '-' && slug.back() != '-' &&
         std::ranges::all_of(slug, [](unsigned char character) {
           return std::islower(character) || std::isdigit(character) || character == '-';
         });
}

[[nodiscard]] inline bool valid_problem_text(std::string_view text) {
  return text.size() <= maximum_problem_text_bytes;
}

[[nodiscard]] inline bool valid_hidden_test_text(std::string_view text) {
  return text.size() <= maximum_hidden_test_bytes;
}

} // namespace algorithm_trainer
