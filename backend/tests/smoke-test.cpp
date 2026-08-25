#include <catch2/catch_test_macros.hpp>

#include <string_view>

TEST_CASE("the test target uses C++20", "[smoke]") {
  constexpr std::string_view project_name{"Algorithm Trainer"};

  REQUIRE(project_name.starts_with("Algorithm"));
}
