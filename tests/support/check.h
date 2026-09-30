#pragma once

#include <cstdlib>
#include <iostream>

namespace halo2_test {
inline const char *scenario = "test setup";
inline void check(bool condition, const char *expression, const char *file, int line) {
  if (condition) return;
  std::cerr << scenario << ": " << file << ':' << line << ": " << expression << '\n';
  std::exit(EXIT_FAILURE);
}
}  // namespace halo2_test

#define CHECK(expression) ::halo2_test::check(bool(expression), #expression, __FILE__, __LINE__)
