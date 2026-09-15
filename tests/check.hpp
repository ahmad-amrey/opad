// Minimal dependency-free test harness: TEST(name) { CHECK(expr); ... } and a main() that runs them all.
#pragma once
#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace check {
struct Case {
  const char* name;
  std::function<void()> fn;
};
inline std::vector<Case>& cases() {
  static std::vector<Case> c;
  return c;
}
inline int& failures() {
  static int f = 0;
  return f;
}
struct Register {
  Register(const char* name, std::function<void()> fn) { cases().push_back({name, std::move(fn)}); }
};
struct Failure : std::exception {
  std::string msg;
  explicit Failure(std::string m) : msg(std::move(m)) {}
  const char* what() const noexcept override { return msg.c_str(); }
};
inline int run_all(int argc, char** argv) {
  std::string filter = argc > 1 ? argv[1] : "";
  int passed = 0, ran = 0;
  for (auto& c : cases()) {
    if (!filter.empty() && std::string(c.name).find(filter) == std::string::npos) continue;
    ++ran;
    try {
      c.fn();
      std::printf("[ ok ] %s\n", c.name);
      ++passed;
    } catch (const std::exception& e) {
      std::printf("[FAIL] %s: %s\n", c.name, e.what());
      ++failures();
    }
  }
  std::printf("%d/%d passed\n", passed, ran);
  return failures() == 0 && ran > 0 ? 0 : 1;
}
}  // namespace check

#define TEST(name)                                                        \
  static void test_##name();                                              \
  static check::Register reg_##name(#name, test_##name);                  \
  static void test_##name()

#define CHECK(expr)                                                                                     \
  do {                                                                                                  \
    if (!(expr)) throw check::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " CHECK(" #expr ")"); \
  } while (0)

#define CHECK_EQ(a, b)                                                                                             \
  do {                                                                                                             \
    if (!((a) == (b)))                                                                                             \
      throw check::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " CHECK_EQ(" #a ", " #b ")"); \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                                                        \
  do {                                                                                                               \
    if (std::fabs(double(a) - double(b)) > (eps))                                                                    \
      throw check::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " CHECK_NEAR(" #a ", " #b ") got " + \
                           std::to_string(double(a)) + " vs " + std::to_string(double(b)));                            \
  } while (0)

#define CHECK_THROWS(expr)                                                                                     \
  do {                                                                                                         \
    bool threw_ = false;                                                                                       \
    try {                                                                                                      \
      (void)(expr);                                                                                            \
    } catch (const std::exception&) {                                                                          \
      threw_ = true;                                                                                           \
    }                                                                                                          \
    if (!threw_) throw check::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " expected throw: " #expr); \
  } while (0)

#define CHECK_MAIN() \
  int main(int argc, char** argv) { return check::run_all(argc, argv); }
