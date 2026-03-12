#pragma once

#include "Error.h"

#include <cassert>
#include <optional>
#include <variant>

namespace spw
{

// Monadic result type for disk operations.
// Forces callers to handle errors — no exceptions can skip past a partial write.
template <typename T>
class Result
{
public:
    // Construct success
    Result(const T& value) : m_data(value) {}
    Result(T&& value) : m_data(std::move(value)) {}

    // Construct failure
    Result(const ErrorInfo& error) : m_data(error)
    {
        assert(error.isError());
    }

    Result(ErrorInfo&& error) : m_data(std::move(error))
    {
        assert(std::get<ErrorInfo>(m_data).isError());
    }

    bool isOk() const { return std::holds_alternative<T>(m_data); }
    bool isError() const { return std::holds_alternative<ErrorInfo>(m_data); }

    const T& value() const&
    {
        assert(isOk());
        return std::get<T>(m_data);
    }

    T& value() &
    {
        assert(isOk());
        return std::get<T>(m_data);
    }

    T&& value() &&
    {
        assert(isOk());
        return std::get<T>(std::move(m_data));
    }

    const ErrorInfo& error() const
    {
        assert(isError());
        return std::get<ErrorInfo>(m_data);
    }

    // Value or default
    T valueOr(const T& defaultVal) const
    {
        if (isOk())
            return value();
        return defaultVal;
    }

    // Monadic map
    template <typename Func>
    auto map(Func&& func) const -> Result<decltype(func(std::declval<T>()))>
    {
        if (isOk())
            return func(value());
        return error();
    }

    // Monadic flatMap / andThen
    template <typename Func>
    auto andThen(Func&& func) const -> decltype(func(std::declval<T>()))
    {
        if (isOk())
            return func(value());
        return error();
    }

    explicit operator bool() const { return isOk(); }

private:
    std::variant<T, ErrorInfo> m_data;
};

// Specialization for void results
template <>
class Result<void>
{
public:
    Result() : m_error(std::nullopt) {}

    Result(const ErrorInfo& error) : m_error(error)
    {
        assert(error.isError());
    }

    bool isOk() const { return !m_error.has_value(); }
    bool isError() const { return m_error.has_value(); }

    const ErrorInfo& error() const
    {
        assert(isError());
        return m_error.value();
    }

    explicit operator bool() const { return isOk(); }

    static Result ok() { return {}; }

private:
    std::optional<ErrorInfo> m_error;
};

} // namespace spw
