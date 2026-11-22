// Minimal test harness (no dependencies).
#pragma once

#include <cmath>
#include <cstdio>
#include <vector>

namespace inktest {
struct Case {
  const char* name;
  void (*fn)();
};
std::vector<Case>& registry();
extern int failures;
struct Registrar {
  Registrar(const char* n, void (*f)()) { registry().push_back({n, f}); }
};
}  // namespace inktest

#define TEST(name)                                               \
  static void name();                                            \
  static inktest::Registrar registrar_##name(#name, &name);      \
  static void name()

#define CHECK(cond)                                                           \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
      ++inktest::failures;                                                    \
    }                                                                         \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                                    \
  do {                                                                                           \
    double va_ = (a), vb_ = (b);                                                                 \
    if (!(std::fabs(va_ - vb_) <= (eps))) {                                                      \
      std::printf("    FAIL %s:%d: %s = %g, %s = %g (eps %g)\n", __FILE__, __LINE__, #a, va_, #b, \
                  vb_, static_cast<double>(eps));                                                \
      ++inktest::failures;                                                                       \
    }                                                                                            \
  } while (0)

#define REQUIRE(cond)                                                         \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::printf("    FAIL %s:%d: %s (fatal)\n", __FILE__, __LINE__, #cond); \
      ++inktest::failures;                                                    \
      return;                                                                 \
    }                                                                         \
  } while (0)
