#pragma once

#include <string>
#include <utility>

namespace cch::util {

template <typename T>
class Result {
public:
    static Result success(T value) { return Result(true, std::move(value), {}); }
    static Result failure(std::string error) { return Result(false, T{}, std::move(error)); }

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] explicit operator bool() const { return ok_; }
    [[nodiscard]] const T& value() const { return value_; }
    [[nodiscard]] T& value() { return value_; }
    [[nodiscard]] const std::string& error() const { return error_; }

private:
    Result(bool ok, T value, std::string error) : ok_(ok), value_(std::move(value)), error_(std::move(error)) {}

    bool ok_;
    T value_{};
    std::string error_;
};

template <>
class Result<void> {
public:
    static Result success() { return Result(true, {}); }
    static Result failure(std::string error) { return Result(false, std::move(error)); }

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] explicit operator bool() const { return ok_; }
    [[nodiscard]] const std::string& error() const { return error_; }

private:
    Result(bool ok, std::string error) : ok_(ok), error_(std::move(error)) {}

    bool ok_;
    std::string error_;
};

} // namespace cch::util
