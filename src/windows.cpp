#include "spaceledger/windows.hpp"

#include <cstdio>
#include <limits>

namespace spaceledger {
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<std::size_t>(INT_MAX)) throw std::runtime_error("Path is too long");
    const auto length = static_cast<int>(value.size());
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Cannot encode path as UTF-8");
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<std::size_t>(INT_MAX)) throw std::runtime_error("Path is too long");
    const auto length = static_cast<int>(value.size());
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
    if (!count) throw std::runtime_error("Invalid UTF-8");
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, result.data(), count);
    return result;
}

fs::path absolute_path(const fs::path& path) {
    if (path.empty()) throw std::runtime_error("Path cannot be empty");
    const auto s = path.native();
    if (s.size() == 2 && s[1] == L':') throw std::runtime_error("Use a drive root such as C:\\, not drive-relative C:");
    return fs::absolute(path).lexically_normal();
}

std::wstring extended(const fs::path& path) {
    const auto value = absolute_path(path).native();
    if (value.starts_with(L"\\\\?\\")) return value;
    if (value.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}

std::string utc_now() {
    SYSTEMTIME t{};
    GetSystemTime(&t);
    char buffer[40]{};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                  t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return buffer;
}

std::string windows_error(DWORD error) {
    wchar_t buffer[1024]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                   buffer, static_cast<DWORD>(std::size(buffer)), nullptr);
    auto message = utf8(buffer);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) message.pop_back();
    return "Windows error " + std::to_string(error) + (message.empty() ? "" : ": " + message);
}

bool same_path(const fs::path& first, const fs::path& second) {
    const auto a = extended(first);
    const auto b = extended(second);
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
}
