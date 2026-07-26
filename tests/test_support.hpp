#pragma once

#include <cstdlib>
#include <iostream>

inline void require_test(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "test failed: " << message << '\n';
    std::exit(1);
  }
}
