#pragma once

#include <cmath>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace vanityforge::test {

struct Case final {
  std::string name;
  std::function<void()> function;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

struct Registrar final {
  Registrar(const char* name, std::function<void()> function) {
    registry().push_back(Case{name, std::move(function)});
  }
};

template <typename Actual, typename Expected>
void require_equal(const Actual& actual, const Expected& expected, const char* actual_text,
                   const char* expected_text, const char* file, int line) {
  if (!(actual == expected)) {
    std::ostringstream message;
    message << file << ':' << line << ": expected " << actual_text << " == " << expected_text;
    throw std::runtime_error(message.str());
  }
}

inline void require_true(bool condition, const char* text, const char* file, int line) {
  if (!condition) {
    std::ostringstream message;
    message << file << ':' << line << ": expected true: " << text;
    throw std::runtime_error(message.str());
  }
}

inline void require_near(long double actual, long double expected, long double tolerance,
                         const char* file, int line) {
  if (std::fabs(actual - expected) > tolerance) {
    std::ostringstream message;
    message << file << ':' << line << ": values differ: " << actual << " vs " << expected;
    throw std::runtime_error(message.str());
  }
}

}  // namespace vanityforge::test

#define VF_TEST_CONCAT_IMPL(a, b) a##b
#define VF_TEST_CONCAT(a, b) VF_TEST_CONCAT_IMPL(a, b)
#define VF_TEST(name)                                                                           \
  static void VF_TEST_CONCAT(vf_test_function_, __LINE__)();                                    \
  static ::vanityforge::test::Registrar VF_TEST_CONCAT(vf_test_registrar_, __LINE__)(           \
      name, VF_TEST_CONCAT(vf_test_function_, __LINE__));                                       \
  static void VF_TEST_CONCAT(vf_test_function_, __LINE__)()
#define VF_REQUIRE(value) ::vanityforge::test::require_true((value), #value, __FILE__, __LINE__)
#define VF_REQUIRE_EQ(actual, expected)                                                         \
  ::vanityforge::test::require_equal((actual), (expected), #actual, #expected, __FILE__, __LINE__)
#define VF_REQUIRE_NEAR(actual, expected, tolerance)                                            \
  ::vanityforge::test::require_near((actual), (expected), (tolerance), __FILE__, __LINE__)

