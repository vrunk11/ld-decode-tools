// SPDX-License-Identifier: GPL-3.0-or-later
//
// check.h — a dependency-free test harness. Each test is a plain executable
// that returns non-zero on the first failed check (CTest reads the exit code).
// Kept tiny on purpose: the engine tests must run in CI without pulling a test
// framework, matching decode-orc's "unit tests are hermetic" rule.

#ifndef LDDECODE_HVD_TESTS_CHECK_H_
#define LDDECODE_HVD_TESTS_CHECK_H_

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace hvdtest {

inline int& failures() {
  static int f = 0;
  return f;
}

inline void ReportOk(const char* expr, const char* file, int line) {
  (void)expr;
  (void)file;
  (void)line;
}

inline void ReportFail(const char* expr, const char* file, int line) {
  std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", expr, file, line);
  ++failures();
}

}  // namespace hvdtest

#define CHECK(cond)                                             \
  do {                                                          \
    if (cond) {                                                 \
      ::hvdtest::ReportOk(#cond, __FILE__, __LINE__);           \
    } else {                                                    \
      ::hvdtest::ReportFail(#cond, __FILE__, __LINE__);         \
    }                                                           \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                              \
  do {                                                                     \
    const double aa = (a);                                                 \
    const double bb = (b);                                                 \
    if (std::fabs(aa - bb) <= (tol)) {                                     \
      ::hvdtest::ReportOk(#a " ~= " #b, __FILE__, __LINE__);               \
    } else {                                                               \
      std::fprintf(stderr, "  %.9g vs %.9g (tol %.3g)\n", aa, bb,          \
                   static_cast<double>(tol));                              \
      ::hvdtest::ReportFail(#a " ~= " #b, __FILE__, __LINE__);             \
    }                                                                      \
  } while (0)

#define TEST_MAIN()                                    \
  int main() {                                          \
    RunTests();                                         \
    if (::hvdtest::failures() == 0) {                   \
      std::printf("OK\n");                              \
      return 0;                                         \
    }                                                   \
    std::fprintf(stderr, "%d failure(s)\n",             \
                 ::hvdtest::failures());                \
    return 1;                                           \
  }

#endif  // LDDECODE_HVD_TESTS_CHECK_H_
