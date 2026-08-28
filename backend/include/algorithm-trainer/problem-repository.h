#pragma once

#include "algorithm-trainer/problem.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace algorithm_trainer {

struct ProblemRepositoryError {
  std::string message;
};

using ProblemListResult = std::variant<std::vector<Problem>, ProblemRepositoryError>;
using ProblemFindResult = std::variant<std::optional<Problem>, ProblemRepositoryError>;
using ProblemWriteResult = std::variant<Problem, ProblemRepositoryError>;
using ProblemDeleteResult = std::variant<bool, ProblemRepositoryError>;
using ProblemTestWriteResult = std::variant<ProblemTestCase, ProblemRepositoryError>;

class ProblemRepository {
public:
  virtual ~ProblemRepository() = default;
  virtual ProblemListResult list_enabled() = 0;
  virtual ProblemFindResult find_enabled(const std::string &id) = 0;
  virtual ProblemFindResult find_for_judging(const std::string &id) = 0;
  virtual ProblemListResult list_all() { return ProblemRepositoryError{"Unsupported operation"}; }
  virtual ProblemFindResult find_any(const std::string &) {
    return ProblemRepositoryError{"Unsupported operation"};
  }
  virtual ProblemWriteResult create(const Problem &) {
    return ProblemRepositoryError{"Unsupported operation"};
  }
  virtual ProblemWriteResult update(const Problem &) {
    return ProblemRepositoryError{"Unsupported operation"};
  }
  virtual ProblemDeleteResult remove(const std::string &) {
    return ProblemRepositoryError{"Unsupported operation"};
  }
  virtual ProblemTestWriteResult create_test(const std::string &, const ProblemTestCase &) {
    return ProblemRepositoryError{"Unsupported operation"};
  }
  virtual ProblemTestWriteResult update_test(const std::string &, const ProblemTestCase &) {
    return ProblemRepositoryError{"Unsupported operation"};
  }
  virtual ProblemDeleteResult remove_test(const std::string &, std::int64_t) {
    return ProblemRepositoryError{"Unsupported operation"};
  }
};

} // namespace algorithm_trainer
