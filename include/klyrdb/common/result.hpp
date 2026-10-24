#pragma once

#include "klyrdb/common/status.hpp"
#include <variant>
#include <utility>
#include <stdexcept>

namespace klyrdb {

/// A container that holds either a successful value T or a failure Status.
template <typename T>
class Result {
public:
    // Implicit conversion from value
    Result(T value) : storage_(std::move(value)) {} // NOLINT

    // Implicit conversion from failure status
    Result(Status status) : storage_(std::move(status)) { // NOLINT
        if (std::get<Status>(storage_).is_ok()) {
            throw std::logic_error("Cannot construct an error Result with Status::Ok");
        }
    }

    [[nodiscard]] bool is_ok() const noexcept {
        return std::holds_alternative<T>(storage_);
    }

    [[nodiscard]] bool is_err() const noexcept {
        return std::holds_alternative<Status>(storage_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return is_ok();
    }

    [[nodiscard]] const T& value() const & {
        if (is_err()) {
            throw std::runtime_error("Attempted to access value on error Result: " +
                                     std::get<Status>(storage_).to_string());
        }
        return std::get<T>(storage_);
    }

    [[nodiscard]] T& value() & {
        if (is_err()) {
            throw std::runtime_error("Attempted to access value on error Result: " +
                                     std::get<Status>(storage_).to_string());
        }
        return std::get<T>(storage_);
    }

    [[nodiscard]] T&& value() && {
        if (is_err()) {
            throw std::runtime_error("Attempted to access value on error Result: " +
                                     std::get<Status>(storage_).to_string());
        }
        return std::get<T>(std::move(storage_));
    }

    [[nodiscard]] const Status& status() const noexcept {
        static const Status ok_status = Status::ok();
        if (is_ok()) {
            return ok_status;
        }
        return std::get<Status>(storage_);
    }

    [[nodiscard]] T value_or(T default_value) const & {
        if (is_ok()) {
            return std::get<T>(storage_);
        }
        return default_value;
    }

    [[nodiscard]] T value_or(T default_value) && {
        if (is_ok()) {
            return std::get<T>(std::move(storage_));
        }
        return default_value;
    }

private:
    std::variant<T, Status> storage_;
};

} // namespace klyrdb
