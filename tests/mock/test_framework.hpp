#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <chrono>
#include <exception>

namespace rtl_test {

class TestFailureException : public std::exception {
public:
    TestFailureException(const std::string& msg, const char* file, int line)
        : message_(msg + " at " + file + ":" + std::to_string(line)) {}
    const char* what() const noexcept override {
        return message_.c_str();
    }
private:
    std::string message_;
};

struct TestCase {
    std::string category;
    std::string name;
    std::function<void()> func;
    std::string file;
    int line;
};

class TestRegistry {
public:
    static TestRegistry& instance() {
        static TestRegistry reg;
        return reg;
    }

    void add_test(const std::string& category, const std::string& name,
                  std::function<void()> func, const char* file, int line) {
        tests_.push_back({category, name, func, file, line});
    }

    const std::vector<TestCase>& tests() const {
        return tests_;
    }

private:
    std::vector<TestCase> tests_;
};

struct AutoRegister {
    AutoRegister(const std::string& category, const std::string& name,
                 std::function<void()> func, const char* file, int line) {
        TestRegistry::instance().add_test(category, name, func, file, line);
    }
};

} // namespace rtl_test

#define REGISTER_TEST(Category, Name) \
    static void test_##Category##_##Name(); \
    static ::rtl_test::AutoRegister auto_reg_##Category##_##Name( \
        #Category, #Name, test_##Category##_##Name, __FILE__, __LINE__); \
    static void test_##Category##_##Name()

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#endif

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            throw ::rtl_test::TestFailureException("ASSERT_TRUE failed: (" #cond ") is false", __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_FALSE(cond) \
    do { \
        if (cond) { \
            throw ::rtl_test::TestFailureException("ASSERT_FALSE failed: (" #cond ") is true", __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_EQ(a, b) \
    do { \
        auto _val_a = (a); \
        auto _val_b = (b); \
        if (!(_val_a == _val_b)) { \
            std::ostringstream _oss; \
            _oss << "ASSERT_EQ failed: " #a " == " #b " (" << _val_a << " vs " << _val_b << ")"; \
            throw ::rtl_test::TestFailureException(_oss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_NE(a, b) \
    do { \
        auto _val_a = (a); \
        auto _val_b = (b); \
        if (_val_a == _val_b) { \
            std::ostringstream _oss; \
            _oss << "ASSERT_NE failed: " #a " != " #b " (" << _val_a << " == " << _val_b << ")"; \
            throw ::rtl_test::TestFailureException(_oss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_GT(a, b) \
    do { \
        auto _val_a = (a); \
        auto _val_b = (b); \
        if (!(_val_a > _val_b)) { \
            std::ostringstream _oss; \
            _oss << "ASSERT_GT failed: " #a " > " #b " (" << _val_a << " <= " << _val_b << ")"; \
            throw ::rtl_test::TestFailureException(_oss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_GE(a, b) \
    do { \
        auto _val_a = (a); \
        auto _val_b = (b); \
        if (!(_val_a >= _val_b)) { \
            std::ostringstream _oss; \
            _oss << "ASSERT_GE failed: " #a " >= " #b " (" << _val_a << " < " << _val_b << ")"; \
            throw ::rtl_test::TestFailureException(_oss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_LT(a, b) \
    do { \
        auto _val_a = (a); \
        auto _val_b = (b); \
        if (!(_val_a < _val_b)) { \
            std::ostringstream _oss; \
            _oss << "ASSERT_LT failed: " #a " < " #b " (" << _val_a << " >= " << _val_b << ")"; \
            throw ::rtl_test::TestFailureException(_oss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_LE(a, b) \
    do { \
        auto _val_a = (a); \
        auto _val_b = (b); \
        if (!(_val_a <= _val_b)) { \
            std::ostringstream _oss; \
            _oss << "ASSERT_LE failed: " #a " <= " #b " (" << _val_a << " > " << _val_b << ")"; \
            throw ::rtl_test::TestFailureException(_oss.str(), __FILE__, __LINE__); \
        } \
    } while (0)

#define ASSERT_MEMEQ(ptr_a, ptr_b, size) \
    do { \
        const uint8_t* _p_a = reinterpret_cast<const uint8_t*>(ptr_a); \
        const uint8_t* _p_b = reinterpret_cast<const uint8_t*>(ptr_b); \
        size_t _sz = (size); \
        if (std::memcmp(_p_a, _p_b, _sz) != 0) { \
            std::ostringstream _oss; \
            _oss << "ASSERT_MEMEQ failed at byte mismatch (size " << _sz << "): "; \
            for (size_t _i = 0; _i < _sz && _i < 16; ++_i) { \
                _oss << std::hex << std::setw(2) << std::setfill('0') << (int)_p_a[_i] << " vs " \
                     << std::setw(2) << std::setfill('0') << (int)_p_b[_i] << " "; \
            } \
            throw ::rtl_test::TestFailureException(_oss.str(), __FILE__, __LINE__); \
        } \
    } while (0)
