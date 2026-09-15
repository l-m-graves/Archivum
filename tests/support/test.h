// Minimal test framework. Vendored-in-spirit: no dependency, ~100 lines.
//
//   ARCHIVUM_TEST(name) { CHECK(x == y); REQUIRE(ptr != nullptr); }
//
// CHECK records a failure and continues; REQUIRE records and aborts the test.
// The binary runs every test, or those whose name contains argv[1].
#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace archivum::testing {

struct TestCase {
  const char* name;
  void (*fn)();
};

std::vector<TestCase>& registry();

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

struct RequireFailed {};

void record_failure(const char* file, int line, const std::string& expr, const std::string& detail);

// Set by tests that want extra context printed with every failure (e.g. the
// seed of a randomized run).
void set_context(const std::string& context);

}  // namespace archivum::testing

#define ARCHIVUM_TEST(name)                                                    \
  static void name();                                                          \
  static ::archivum::testing::Registrar archivum_registrar_##name(#name, &name); \
  static void name()

#define CHECK(expr)                                                                   \
  do {                                                                                \
    if (!(expr)) ::archivum::testing::record_failure(__FILE__, __LINE__, #expr, ""); \
  } while (0)

#define CHECK_MSG(expr, msg)                                                                  \
  do {                                                                                        \
    if (!(expr)) {                                                                            \
      std::ostringstream archivum_oss;                                                        \
      archivum_oss << msg;                                                                    \
      ::archivum::testing::record_failure(__FILE__, __LINE__, #expr, archivum_oss.str());    \
    }                                                                                         \
  } while (0)

#define REQUIRE(expr)                                                              \
  do {                                                                             \
    if (!(expr)) {                                                                 \
      ::archivum::testing::record_failure(__FILE__, __LINE__, #expr, "");         \
      throw ::archivum::testing::RequireFailed{};                                  \
    }                                                                              \
  } while (0)

#define REQUIRE_MSG(expr, msg)                                                             \
  do {                                                                                     \
    if (!(expr)) {                                                                         \
      std::ostringstream archivum_oss;                                                     \
      archivum_oss << msg;                                                                 \
      ::archivum::testing::record_failure(__FILE__, __LINE__, #expr, archivum_oss.str()); \
      throw ::archivum::testing::RequireFailed{};                                          \
    }                                                                                      \
  } while (0)

#define REQUIRE_OK(status_expr)                                                            \
  do {                                                                                     \
    const auto archivum_st = (status_expr);                                                \
    if (!archivum_st.ok()) {                                                               \
      ::archivum::testing::record_failure(__FILE__, __LINE__, #status_expr,               \
                                          archivum_st.to_string());                        \
      throw ::archivum::testing::RequireFailed{};                                          \
    }                                                                                      \
  } while (0)
