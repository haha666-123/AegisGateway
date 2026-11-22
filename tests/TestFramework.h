#pragma once

// Minimal, dependency free test harness.
//
//   AEGIS_TEST(some_name) { CHECK_EQ(2 + 2, 4); }
//
// Every test is registered statically; test_main runs them all (optionally
// filtered with --filter=substr) and returns a non zero exit code on failure so
// `ctest` and CI can gate on it.

#include <atomic>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace aegis::test {

struct Case {
    const char* name;
    void (*fn)();
};

class Registry {
public:
    static Registry& instance() {
        static Registry registry;
        return registry;
    }

    void add(const char* name, void (*fn)()) { cases_.push_back(Case{name, fn}); }

    int run(std::string_view filter, bool list_only) {
        int failed = 0;
        int executed = 0;
        for (const auto& test_case : cases_) {
            if (!filter.empty() && std::string_view(test_case.name).find(filter) ==
                                       std::string_view::npos) {
                continue;
            }
            if (list_only) {
                std::cout << test_case.name << '\n';
                continue;
            }
            ++executed;
            current_failures_ = 0;
            const int before = g_assertions.load();
            try {
                test_case.fn();
            } catch (const std::exception& ex) {
                ++current_failures_;
                std::cerr << "  [exception] " << ex.what() << '\n';
            } catch (...) {
                ++current_failures_;
                std::cerr << "  [exception] unknown\n";
            }
            const int checks = g_assertions.load() - before;
            if (current_failures_ == 0) {
                std::cout << "[ OK ] " << test_case.name << " (" << checks << " checks)\n";
            } else {
                std::cout << "[FAIL] " << test_case.name << " (" << current_failures_
                          << " failure(s))\n";
                ++failed;
            }
        }
        if (!list_only) {
            std::cout << "\n" << (executed - failed) << '/' << executed << " tests passed, "
                      << g_assertions.load() << " assertions\n";
        }
        return failed == 0 ? 0 : 1;
    }

    void report_failure(const char* file, int line, const std::string& detail) {
        ++current_failures_;
        std::cerr << "  " << file << ':' << line << " " << detail << '\n';
    }

    static std::atomic<int>& assertion_counter() { return g_assertions; }

    int current_failures_{0};

private:
    std::vector<Case> cases_;
    static inline std::atomic<int> g_assertions{0};
};

struct Registrar {
    Registrar(const char* name, void (*fn)()) { Registry::instance().add(name, fn); }
};

template <class T>
std::string to_text(const T& value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
}

inline std::string to_text(const std::string& value) { return value; }
inline std::string to_text(std::string_view value) { return std::string(value); }
inline std::string to_text(const char* value) { return std::string(value); }

}  // namespace aegis::test

#define AEGIS_TEST_IMPL(name, counter)                                    \
    static void name##_impl();                                            \
    static ::aegis::test::Registrar name##_registrar(#name, &name##_impl); \
    static void name##_impl()

#define AEGIS_TEST(name) AEGIS_TEST_IMPL(name, __COUNTER__)

#define AEGIS_CHECK(cond)                                                          \
    do {                                                                           \
        ::aegis::test::Registry::assertion_counter().fetch_add(1);                 \
        if (!(cond)) {                                                             \
            ::aegis::test::Registry::instance().report_failure(__FILE__, __LINE__, \
                                                               "CHECK failed: " #cond); \
        }                                                                          \
    } while (false)

#define AEGIS_CHECK_EQ(lhs, rhs)                                                             \
    do {                                                                                     \
        ::aegis::test::Registry::assertion_counter().fetch_add(1);                           \
        const auto aegis_lhs = (lhs);                                                       \
        const auto aegis_rhs = (rhs);                                                       \
        if (!(aegis_lhs == aegis_rhs)) {                                                     \
            ::aegis::test::Registry::instance().report_failure(                              \
                __FILE__, __LINE__,                                                          \
                std::string("CHECK_EQ failed: " #lhs " == " #rhs " | left=") +               \
                    ::aegis::test::to_text(aegis_lhs) + " right=" +                          \
                    ::aegis::test::to_text(aegis_rhs));                                      \
        }                                                                                    \
    } while (false)

#define AEGIS_CHECK_NE(lhs, rhs) \
    do {                         \
        ::aegis::test::Registry::assertion_counter().fetch_add(1); \
        if ((lhs) == (rhs)) {    \
            ::aegis::test::Registry::instance().report_failure(    \
                __FILE__, __LINE__, "CHECK_NE failed: " #lhs " != " #rhs); \
        }                        \
    } while (false)
