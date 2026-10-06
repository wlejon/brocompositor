// Minimal test harness: checks are real code paths in every configuration
// (no assert()), failures are counted and reported, and main() returns
// nonzero when anything failed.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace bctest {

inline int& failures() {
    static int n = 0;
    return n;
}

inline void fail(const char* file, int line, const std::string& what) {
    ++failures();
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what.c_str());
    std::fflush(stderr);
}

template <class A, class B>
std::string describe(const char* ea, const char* eb, const A& a, const B& b) {
    std::ostringstream s;
    s << ea << " == " << eb << " (got " << a << " vs " << b << ")";
    return s.str();
}

inline int finish(const char* name) {
    if (failures() == 0) {
        std::printf("[%s] PASSED\n", name);
        return 0;
    }
    std::printf("[%s] FAILED (%d check%s)\n", name, failures(), failures() == 1 ? "" : "s");
    return 1;
}

// Tests that act on the user's real desktop (reserving work-area strips,
// which moves the user's maximized windows; taking the foreground from the
// user) run only with BROCOMPOSITOR_TEST_MUTATE=1. CI runners are disposable
// and set it. Otherwise the test skips (exit 77) and says why.
inline void require_mutate(const char* name, const char* what) {
    const char* v = std::getenv("BROCOMPOSITOR_TEST_MUTATE");
    if (v && std::string(v) == "1") return;
    std::printf("[%s] SKIPPED: %s on the user's real desktop; set BROCOMPOSITOR_TEST_MUTATE=1 to run it\n",
                name, what);
    std::fflush(stdout);
    std::exit(77);
}

}  // namespace bctest

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) ::bctest::fail(__FILE__, __LINE__, #cond);        \
    } while (0)

#define CHECK_EQ(a, b)                                                                 \
    do {                                                                               \
        auto check_a_ = (a); /* copies: operands may reference temporaries */           \
        auto check_b_ = (b);                                                           \
        if (!(check_a_ == check_b_))                                                   \
            ::bctest::fail(__FILE__, __LINE__,                                         \
                           ::bctest::describe(#a, #b, check_a_, check_b_));            \
    } while (0)

// Fatal variant: stops the current test function.
#define REQUIRE(cond)                                                  \
    do {                                                               \
        if (!(cond)) {                                                 \
            ::bctest::fail(__FILE__, __LINE__, "required: " #cond);    \
            return;                                                    \
        }                                                              \
    } while (0)
