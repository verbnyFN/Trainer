#include "algorithm-trainer/judge.h"

#include <stdexcept>

namespace algorithm_trainer {

std::string_view verdict_name(Verdict verdict) {
  switch (verdict) {
  case Verdict::accepted:
    return "Accepted";
  case Verdict::wrong_answer:
    return "Wrong Answer";
  case Verdict::runtime_error:
    return "Runtime Error";
  case Verdict::time_limit_exceeded:
    return "Time Limit Exceeded";
  }

  throw std::logic_error{"Unknown verdict"};
}

} // namespace algorithm_trainer
