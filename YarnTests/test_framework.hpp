//
// Created by Fabrizio Paino on 2026-05-15.
//

#ifndef YARN_TEST_FRAMEWORK_HPP
#define YARN_TEST_FRAMEWORK_HPP

#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace test {

    /**
     * @brief Exception type thrown by EXPECT_* macros on failure. The test
     *        runner catches it and records the failure with the assertion's
     *        file:line context.
     */
    struct TestFailure : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    /**
     * @struct TestCase
     * @brief Bundle of a test's display name and its callable body.
     */
    struct TestCase {
        const char *name;
        std::function<void()> fn;
    };

    /**
     * @brief Global registry of test cases. Populated at static-init time by
     *        Registrar instances created by the TEST() macro.
     */
    inline std::vector<TestCase> &registry() {
        static std::vector<TestCase> v;
        return v;
    }

    /**
     * @struct Registrar
     * @brief Helper that pushes a TestCase into @ref registry on construction.
     *        Instantiated as a static by the TEST() macro.
     */
    struct Registrar {
        Registrar(const char *name, std::function<void()> fn) {
            registry().push_back({name, std::move(fn)});
        }
    };

    /**
     * @brief Run every registered test in registration order. Prints a
     *        per-test status line and a final tally.
     * @return 0 if every test passed, 1 if at least one failed.
     */
    inline int run_all() {
        int passed = 0;
        int failed = 0;
        std::vector<std::string> failures;
        for (const auto &tc : registry()) {
            auto start = std::chrono::steady_clock::now();
            std::cout << "[ RUN  ] " << tc.name << std::endl;
            try {
                tc.fn();
                auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count();
                std::cout << "[  OK  ] " << tc.name << " (" << us << " us)" << std::endl;
                ++passed;
            } catch (const std::exception &e) {
                std::cout << "[ FAIL ] " << tc.name << ": " << e.what() << std::endl;
                ++failed;
                failures.emplace_back(tc.name);
            } catch (...) {
                std::cout << "[ FAIL ] " << tc.name << ": unknown exception" << std::endl;
                ++failed;
                failures.emplace_back(tc.name);
            }
        }
        std::cout << "\n=== " << passed << " passed, " << failed << " failed ===\n";
        if (failed > 0) {
            std::cout << "Failed:\n";
            for (const auto &f : failures) std::cout << "  - " << f << "\n";
        }
        return failed == 0 ? 0 : 1;
    }

} // namespace test


/**
 * @def TEST(name)
 * @brief Define a test case. Registers it with the test runner at static-
 *        init time. Use as @code TEST(my_test) { ... } @endcode.
 */
#define TEST_CONCAT_(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT_(a, b)
#define TEST(name)                                                            \
    static void TEST_CONCAT(test_fn_, name)();                                \
    static ::test::Registrar TEST_CONCAT(test_reg_, name){                    \
        #name, &TEST_CONCAT(test_fn_, name)};                                 \
    static void TEST_CONCAT(test_fn_, name)()

/**
 * @def EXPECT_TRUE(cond)
 * @brief Assert that @p cond evaluates to @c true.
 */
#define EXPECT_TRUE(cond)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::ostringstream _os;                                           \
            _os << __FILE__ << ":" << __LINE__                                \
                << " EXPECT_TRUE(" #cond ") failed";                          \
            throw ::test::TestFailure(_os.str());                             \
        }                                                                     \
    } while (0)

/**
 * @def EXPECT_FALSE(cond)
 * @brief Assert that @p cond evaluates to @c false.
 */
#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))

/**
 * @def EXPECT_EQ(a, b)
 * @brief Assert @c a == @c b. Both operands must be streamable to ostream.
 */
#define EXPECT_EQ(a, b)                                                       \
    do {                                                                      \
        auto &&_a = (a);                                                      \
        auto &&_b = (b);                                                      \
        if (!(_a == _b)) {                                                    \
            std::ostringstream _os;                                           \
            _os << __FILE__ << ":" << __LINE__                                \
                << " EXPECT_EQ(" #a ", " #b ") failed: " << _a << " != " << _b; \
            throw ::test::TestFailure(_os.str());                             \
        }                                                                     \
    } while (0)

/**
 * @def EXPECT_NE(a, b)
 * @brief Assert @c a != @c b.
 */
#define EXPECT_NE(a, b)                                                       \
    do {                                                                      \
        auto &&_a = (a);                                                      \
        auto &&_b = (b);                                                      \
        if (_a == _b) {                                                       \
            std::ostringstream _os;                                           \
            _os << __FILE__ << ":" << __LINE__                                \
                << " EXPECT_NE(" #a ", " #b ") failed: both = " << _a;        \
            throw ::test::TestFailure(_os.str());                             \
        }                                                                     \
    } while (0)

/**
 * @def EXPECT_THROWS_AS(expr, ExceptType)
 * @brief Evaluate @p expr and assert that it throws an exception convertible
 *        to @p ExceptType.
 */
#define EXPECT_THROWS_AS(expr, ExceptType)                                    \
    do {                                                                      \
        bool _threw_correct = false;                                          \
        try {                                                                 \
            (void) (expr);                                                    \
        } catch (const ExceptType &) {                                        \
            _threw_correct = true;                                            \
        } catch (...) {                                                       \
            std::ostringstream _os;                                           \
            _os << __FILE__ << ":" << __LINE__                                \
                << " EXPECT_THROWS_AS(" #expr ", " #ExceptType                \
                   ") threw a different type";                                \
            throw ::test::TestFailure(_os.str());                             \
        }                                                                     \
        if (!_threw_correct) {                                                \
            std::ostringstream _os;                                           \
            _os << __FILE__ << ":" << __LINE__                                \
                << " EXPECT_THROWS_AS(" #expr ", " #ExceptType                \
                   ") did not throw";                                         \
            throw ::test::TestFailure(_os.str());                             \
        }                                                                     \
    } while (0)

#endif // YARN_TEST_FRAMEWORK_HPP
