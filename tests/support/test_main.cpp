#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include "test.h"

namespace archivum::testing {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

namespace {
int g_failures = 0;
std::string g_context;
}  // namespace

void record_failure(const char* file, int line, const std::string& expr, const std::string& detail) {
  ++g_failures;
  std::fprintf(stderr, "  FAILED %s:%d: %s", file, line, expr.c_str());
  if (!detail.empty()) std::fprintf(stderr, "\n    %s", detail.c_str());
  if (!g_context.empty()) std::fprintf(stderr, "\n    context: %s", g_context.c_str());
  std::fprintf(stderr, "\n");
}

void set_context(const std::string& context) { g_context = context; }

}  // namespace archivum::testing

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0;
  int failed_tests = 0;
  for (const auto& tc : archivum::testing::registry()) {
    if (filter != nullptr && std::strstr(tc.name, filter) == nullptr) continue;
    ++run;
    const int before = archivum::testing::g_failures;
    archivum::testing::set_context("");
    std::printf("[ RUN  ] %s\n", tc.name);
    std::fflush(stdout);
    try {
      tc.fn();
    } catch (const archivum::testing::RequireFailed&) {
    } catch (const std::exception& e) {
      archivum::testing::record_failure("<exception>", 0, tc.name, e.what());
    }
    if (archivum::testing::g_failures == before) {
      std::printf("[  OK  ] %s\n", tc.name);
    } else {
      ++failed_tests;
      std::printf("[ FAIL ] %s\n", tc.name);
    }
  }
  std::printf("%d test(s) run, %d failed, %d check(s) failed\n", run, failed_tests,
              archivum::testing::g_failures);
  return archivum::testing::g_failures == 0 && run > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
