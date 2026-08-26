#include "algorithm-trainer/problem.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace algorithm_trainer {
namespace {

using Values = std::vector<long long>;
using Intervals = std::vector<std::pair<long long, long long>>;

std::string values_line(const Values &values) {
  std::ostringstream output;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      output << ' ';
    }
    output << values[index];
  }
  output << '\n';
  return output.str();
}

std::string sized_values_input(const Values &values) {
  return std::to_string(values.size()) + "\n" + values_line(values);
}

ProblemTestCase sort_case(Values values) {
  const auto input = sized_values_input(values);
  std::ranges::sort(values);
  return {.input = input, .expected_output = values_line(values)};
}

std::string intervals_input(const Intervals &intervals) {
  std::ostringstream input;
  input << intervals.size() << '\n';
  for (const auto &[start, end] : intervals) {
    input << start << ' ' << end << '\n';
  }
  return input.str();
}

ProblemTestCase activity_case(Intervals intervals) {
  const auto input = intervals_input(intervals);
  std::ranges::sort(intervals, [](const auto &left, const auto &right) {
    return std::pair{left.second, left.first} < std::pair{right.second, right.first};
  });
  long long last_end = std::numeric_limits<long long>::min();
  std::size_t selected{};
  for (const auto &[start, end] : intervals) {
    if (start >= last_end) {
      ++selected;
      last_end = end;
    }
  }
  return {.input = input, .expected_output = std::to_string(selected) + "\n"};
}

ProblemTestCase cookie_case(Values appetites, Values cookies) {
  std::ostringstream input;
  input << appetites.size() << ' ' << cookies.size() << '\n'
        << values_line(appetites) << values_line(cookies);
  std::ranges::sort(appetites);
  std::ranges::sort(cookies);
  std::size_t child{};
  for (const auto cookie : cookies) {
    if (child < appetites.size() && cookie >= appetites[child]) {
      ++child;
    }
  }
  return {.input = input.str(), .expected_output = std::to_string(child) + "\n"};
}

ProblemTestCase arrows_case(Intervals balloons) {
  const auto input = intervals_input(balloons);
  std::ranges::sort(balloons, [](const auto &left, const auto &right) {
    return std::pair{left.second, left.first} < std::pair{right.second, right.first};
  });
  std::size_t arrows{};
  long long position{};
  for (const auto &[start, end] : balloons) {
    if (arrows == 0 || start > position) {
      ++arrows;
      position = end;
    }
  }
  return {.input = input, .expected_output = std::to_string(arrows) + "\n"};
}

ProblemTestCase distinct_case(const Values &values) {
  std::unordered_map<long long, bool> distinct;
  for (const auto value : values) {
    distinct[value] = true;
  }
  return {.input = sized_values_input(values),
          .expected_output = std::to_string(distinct.size()) + "\n"};
}

ProblemTestCase first_unique_case(const Values &values) {
  std::unordered_map<long long, std::size_t> counts;
  for (const auto value : values) {
    ++counts[value];
  }
  const auto found =
      std::ranges::find_if(values, [&counts](long long value) { return counts.at(value) == 1; });
  const auto answer = found == values.end() ? -1 : *found;
  return {.input = sized_values_input(values), .expected_output = std::to_string(answer) + "\n"};
}

ProblemTestCase pair_sum_case(const Values &values, long long target) {
  std::unordered_map<long long, long long> seen;
  long long pairs{};
  for (const auto value : values) {
    if (const auto found = seen.find(target - value); found != seen.end()) {
      pairs += found->second;
    }
    ++seen[value];
  }
  std::ostringstream input;
  input << values.size() << ' ' << target << '\n' << values_line(values);
  return {.input = input.str(), .expected_output = std::to_string(pairs) + "\n"};
}

Values repeated_values(std::size_t size, long long modulus, long long offset = 0) {
  Values values;
  values.reserve(size);
  for (std::size_t index = 0; index < size; ++index) {
    values.push_back(static_cast<long long>(index % static_cast<std::size_t>(modulus)) + offset);
  }
  return values;
}

Problem make_a_plus_b() {
  Problem problem{
      .id = "a-plus-b",
      .title = "A + B",
      .description = "Read two integers, a and b, and print their sum.",
      .input_format = "One line containing two integers separated by a space.",
      .output_format = "Print one integer: a + b.",
      .difficulty = ProblemDifficulty::easy,
      .tags = {"math", "implementation"},
      .languages = {"python", "cpp"},
      .examples = {{.input = "2 3\n", .output = "5\n"}},
  };
  for (const auto &[input, output] : std::vector<std::pair<std::string, std::string>>{
           {"2 3\n", "5\n"},
           {"0 0\n", "0\n"},
           {"-7 -5\n", "-12\n"},
           {"1000000000 2000000000\n", "3000000000\n"},
           {"-1 1\n", "0\n"},
           {"42 58\n", "100\n"},
           {"-100 40\n", "-60\n"},
           {"9 10\n", "19\n"},
           {"999999999 -999999999\n", "0\n"},
           {"-2000000000 -2000000000\n", "-4000000000\n"}}) {
    problem.hidden_tests.push_back({input, output});
  }
  return problem;
}

Problem make_merge_sort() {
  auto large = repeated_values(30000, 10);
  std::ranges::reverse(large);
  return {
      .id = "merge-sort",
      .title = "Merge Sort",
      .description = "Sort the array in nondecreasing order. Implement an O(n log n) merge-sort "
                     "solution; quadratic sorting is too slow for the largest test.",
      .input_format = "The first line contains n (1 <= n <= 30000). The second line contains n "
                      "integers in the range [-10^9, 10^9].",
      .output_format = "Print the n integers in nondecreasing order, separated by spaces.",
      .difficulty = ProblemDifficulty::medium,
      .tags = {"sorting", "merge-sort", "divide-and-conquer", "arrays"},
      .languages = {"python", "cpp"},
      .examples = {{.input = "5\n4 1 3 2 2\n", .output = "1 2 2 3 4\n"}},
      .hidden_tests =
          {
              sort_case({5}),
              sort_case({2, 1}),
              sort_case({1, 2, 3, 4, 5}),
              sort_case({5, 4, 3, 2, 1}),
              sort_case({4, 1, 3, 2, 2}),
              sort_case({0, -1, 8, -1, 3, 0}),
              sort_case({1000000000, -1000000000, 7, 7, 6}),
              sort_case(repeated_values(1000, 1, 42)),
              sort_case(repeated_values(4096, 97, -48)),
              sort_case(std::move(large)),
          },
  };
}

Problem make_activity_selection() {
  Intervals large;
  large.reserve(40000);
  for (long long index = 0; index < 40000; ++index) {
    large.emplace_back(index % 20000, index % 20000 + 1 + index % 17);
  }
  return {
      .id = "activity-selection",
      .title = "Activity Selection",
      .description = "Choose the maximum number of non-overlapping activities. An activity ending "
                     "when another starts does not overlap it.",
      .input_format = "The first line contains n (1 <= n <= 40000). Each of the next n lines "
                      "contains start and end, where start < end.",
      .output_format = "Print the maximum number of activities that can be selected.",
      .difficulty = ProblemDifficulty::medium,
      .tags = {"greedy", "sorting", "intervals"},
      .languages = {"python", "cpp"},
      .examples = {{.input = "4\n1 3\n2 5\n4 7\n7 8\n", .output = "3\n"}},
      .hidden_tests =
          {
              activity_case({{1, 2}}),
              activity_case({{1, 3}, {2, 4}}),
              activity_case({{1, 3}, {3, 5}, {5, 8}}),
              activity_case({{1, 10}, {2, 3}, {3, 4}, {4, 5}}),
              activity_case({{-5, -2}, {-2, 0}, {-1, 2}}),
              activity_case({{7, 8}, {1, 3}, {4, 7}, {2, 5}}),
              activity_case({{0, 100}, {1, 2}, {2, 3}, {3, 4}, {4, 5}}),
              activity_case({{1, 4}, {1, 2}, {2, 4}, {4, 6}, {5, 7}}),
              activity_case({{3, 9}, {0, 6}, {5, 7}, {8, 9}, {1, 2}, {2, 3}}),
              activity_case(std::move(large)),
          },
  };
}

Problem make_assign_cookies() {
  auto large_appetites = repeated_values(50000, 1000, 1);
  auto large_cookies = repeated_values(50000, 1200, 1);
  std::ranges::reverse(large_cookies);
  return {
      .id = "assign-cookies",
      .title = "Assign Cookies",
      .description =
          "Each child has an appetite and each cookie has a size. A child is satisfied "
          "by one cookie whose size is at least the appetite. Maximize satisfied children.",
      .input_format = "The first line contains n and m (0 <= n,m <= 50000). The next line has n "
                      "appetites and the last line has m cookie sizes.",
      .output_format = "Print the maximum number of children that can be satisfied.",
      .difficulty = ProblemDifficulty::easy,
      .tags = {"greedy", "sorting", "two-pointers"},
      .languages = {"python", "cpp"},
      .examples = {{.input = "3 2\n1 2 3\n1 2\n", .output = "2\n"}},
      .hidden_tests =
          {
              cookie_case({}, {}),
              cookie_case({1}, {}),
              cookie_case({}, {1}),
              cookie_case({1, 2, 3}, {1, 2}),
              cookie_case({5, 5}, {4, 6}),
              cookie_case({3, 1, 2}, {2, 3, 1}),
              cookie_case({10}, {1, 2, 3, 10}),
              cookie_case({1, 1, 1, 1}, {1, 1}),
              cookie_case(repeated_values(2000, 100, 1), repeated_values(1500, 120, 1)),
              cookie_case(std::move(large_appetites), std::move(large_cookies)),
          },
  };
}

Problem make_minimum_arrows() {
  Intervals large;
  large.reserve(50000);
  for (long long index = 0; index < 50000; ++index) {
    large.emplace_back(index * 2, index * 2 + 3);
  }
  return {
      .id = "minimum-arrows",
      .title = "Minimum Arrows for Balloons",
      .description = "Each balloon occupies a closed interval. One arrow shot at coordinate x "
                     "bursts every interval containing x. Find the minimum number of arrows.",
      .input_format = "The first line contains n (1 <= n <= 50000). Each following line contains "
                      "the inclusive left and right endpoints of one balloon.",
      .output_format = "Print the minimum number of arrows needed.",
      .difficulty = ProblemDifficulty::medium,
      .tags = {"greedy", "sorting", "intervals"},
      .languages = {"python", "cpp"},
      .examples = {{.input = "4\n10 16\n2 8\n1 6\n7 12\n", .output = "2\n"}},
      .hidden_tests =
          {
              arrows_case({{1, 2}}),
              arrows_case({{1, 2}, {3, 4}}),
              arrows_case({{1, 2}, {2, 3}}),
              arrows_case({{1, 10}, {2, 9}, {3, 8}}),
              arrows_case({{-10, -5}, {-7, 0}, {1, 2}}),
              arrows_case({{10, 16}, {2, 8}, {1, 6}, {7, 12}}),
              arrows_case({{1, 2}, {4, 5}, {7, 8}, {2, 7}}),
              arrows_case({{0, 0}, {0, 1}, {1, 1}}),
              arrows_case({{-1000000000, 1000000000}, {0, 0}}),
              arrows_case(std::move(large)),
          },
  };
}

Problem make_distinct_values() {
  return {
      .id = "distinct-values",
      .title = "Count Distinct Values",
      .description = "Count how many different integer values occur in the array.",
      .input_format = "The first line contains n (1 <= n <= 100000). The second line contains n "
                      "integers.",
      .output_format = "Print the number of distinct values.",
      .difficulty = ProblemDifficulty::easy,
      .tags = {"hash-table", "set", "arrays"},
      .languages = {"python", "cpp"},
      .examples = {{.input = "6\n1 2 2 3 1 4\n", .output = "4\n"}},
      .hidden_tests =
          {
              distinct_case({1}),
              distinct_case({1, 1, 1}),
              distinct_case({1, 2, 3}),
              distinct_case({-1, 0, 1, -1}),
              distinct_case({1000000000, -1000000000}),
              distinct_case({5, 4, 5, 4, 3, 2, 1}),
              distinct_case(repeated_values(1000, 7)),
              distinct_case(repeated_values(10000, 997, -498)),
              distinct_case(repeated_values(50000, 25000)),
              distinct_case(repeated_values(100000, 60001, -30000)),
          },
  };
}

Problem make_first_unique() {
  auto large = repeated_values(60000, 29999);
  large.insert(large.begin() + 45000, 1000000000);
  return {
      .id = "first-unique",
      .title = "First Unique Value",
      .description = "Find the first array value, in input order, that occurs exactly once.",
      .input_format = "The first line contains n (1 <= n <= 60001). The second line contains n "
                      "integers.",
      .output_format = "Print the first value occurring once, or -1 if no such value exists.",
      .difficulty = ProblemDifficulty::easy,
      .tags = {"hash-table", "frequency", "arrays"},
      .languages = {"python", "cpp"},
      .examples = {{.input = "6\n4 5 4 6 5 7\n", .output = "6\n"}},
      .hidden_tests =
          {
              first_unique_case({1}),
              first_unique_case({1, 1}),
              first_unique_case({1, 2, 2}),
              first_unique_case({2, 2, 1}),
              first_unique_case({4, 5, 4, 6, 5, 7}),
              first_unique_case({-1, 0, -1, 2, 0}),
              first_unique_case({3, 3, 2, 2, 1}),
              first_unique_case(repeated_values(1000, 500)),
              first_unique_case({1000000000, -1000000000, 1000000000}),
              first_unique_case(std::move(large)),
          },
  };
}

Problem make_pair_sum_count() {
  auto large = repeated_values(100000, 2001, -1000);
  return {
      .id = "pair-sum-count",
      .title = "Count Pairs With a Given Sum",
      .description = "Count index pairs (i, j) with i < j whose values sum to the target. Equal "
                     "values at different indices form different pairs.",
      .input_format = "The first line contains n and target (1 <= n <= 100000). The second line "
                      "contains n integers.",
      .output_format =
          "Print the number of valid index pairs. The answer fits in a signed 64-bit integer.",
      .difficulty = ProblemDifficulty::medium,
      .tags = {"hash-table", "frequency", "two-sum", "counting"},
      .languages = {"python", "cpp"},
      .examples = {{.input = "5 6\n1 5 3 3 5\n", .output = "4\n"}},
      .hidden_tests =
          {
              pair_sum_case({1}, 2),
              pair_sum_case({1, 1}, 2),
              pair_sum_case({1, 2, 3}, 4),
              pair_sum_case({1, 5, 3, 3, 5}, 6),
              pair_sum_case({-1, 1, 0, 0}, 0),
              pair_sum_case({5, 5, 5, 5}, 10),
              pair_sum_case({1, 2, 3, 4, 5}, 100),
              pair_sum_case(repeated_values(1000, 10), 9),
              pair_sum_case(repeated_values(20000, 101, -50), 0),
              pair_sum_case(std::move(large), 0),
          },
  };
}

} // namespace

std::string_view difficulty_name(ProblemDifficulty difficulty) {
  switch (difficulty) {
  case ProblemDifficulty::easy:
    return "Easy";
  case ProblemDifficulty::medium:
    return "Medium";
  case ProblemDifficulty::hard:
    return "Hard";
  }
  throw std::logic_error{"Unknown problem difficulty"};
}

const std::vector<Problem> &all_problems() {
  static const std::vector problems{
      make_a_plus_b(),       make_merge_sort(),     make_activity_selection(),
      make_assign_cookies(), make_minimum_arrows(), make_distinct_values(),
      make_first_unique(),   make_pair_sum_count(),
  };
  return problems;
}

const Problem *find_problem(std::string_view slug) {
  const auto &problems = all_problems();
  const auto found = std::ranges::find(problems, slug, &Problem::id);
  return found == problems.end() ? nullptr : &*found;
}

Json::Value problem_summary_to_json(const Problem &problem) {
  Json::Value json;
  json["id"] = problem.id;
  json["title"] = problem.title;
  json["difficulty"] = std::string{difficulty_name(problem.difficulty)};
  json["tags"] = Json::Value{Json::arrayValue};
  for (const auto &tag : problem.tags) {
    json["tags"].append(tag);
  }
  return json;
}

Json::Value problem_to_json(const Problem &problem) {
  Json::Value json;
  json["id"] = problem.id;
  json["title"] = problem.title;
  json["description"] = problem.description;
  json["inputFormat"] = problem.input_format;
  json["outputFormat"] = problem.output_format;
  json["difficulty"] = std::string{difficulty_name(problem.difficulty)};
  json["tags"] = problem_summary_to_json(problem)["tags"];

  Json::Value languages{Json::arrayValue};
  for (const auto &language : problem.languages) {
    languages.append(language);
  }
  json["languages"] = std::move(languages);

  Json::Value examples{Json::arrayValue};
  for (const auto &example : problem.examples) {
    Json::Value serialized_example;
    serialized_example["input"] = example.input;
    serialized_example["output"] = example.output;
    examples.append(std::move(serialized_example));
  }
  json["examples"] = std::move(examples);
  return json;
}

} // namespace algorithm_trainer
