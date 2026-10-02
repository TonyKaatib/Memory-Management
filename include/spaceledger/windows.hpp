#pragma once

#include "model.hpp"
#include <Windows.h>
#include <stdexcept>

namespace spaceledger {
class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { if (*this) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }
private:
    HANDLE value_;
};

std::string utf8(const std::wstring& value);
std::wstring wide(const std::string& value);
fs::path absolute_path(const fs::path& path);
std::wstring extended(const fs::path& path);
std::string utc_now();
std::string windows_error(DWORD error);
bool same_path(const fs::path& first, const fs::path& second);
}
