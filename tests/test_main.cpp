#include "test_framework.h"

#include <exception>
#include <iostream>

int main() {
  std::size_t passed = 0;
  for (const auto& test_case : vanityforge::test::registry()) {
    try {
      test_case.function();
      ++passed;
      std::cout << "[PASS] " << test_case.name << '\n';
    } catch (const std::exception& error) {
      std::cerr << "[FAIL] " << test_case.name << ": " << error.what() << '\n';
    } catch (...) {
      std::cerr << "[FAIL] " << test_case.name << ": unknown exception\n";
    }
  }
  const std::size_t failed = vanityforge::test::registry().size() - passed;
  std::cout << passed << " passed, " << failed << " failed\n";
  return failed == 0 ? 0 : 1;
}

