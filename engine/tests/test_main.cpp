#include <cstring>

#include "test.h"

namespace inktest {
std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}
int failures = 0;
}  // namespace inktest

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0, failedCases = 0;
  for (const inktest::Case& c : inktest::registry()) {
    if (filter && !std::strstr(c.name, filter)) continue;
    int before = inktest::failures;
    c.fn();
    ++run;
    bool ok = inktest::failures == before;
    if (!ok) ++failedCases;
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", c.name);
  }
  std::printf("\n%d tests, %d failed (%d failed checks)\n", run, failedCases, inktest::failures);
  return inktest::failures == 0 ? 0 : 1;
}
