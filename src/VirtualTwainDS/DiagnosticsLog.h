#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace mbf::twain::diagnostics
{

inline std::wstring LogFilePath()
{
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;)
    {
        const DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (length == 0)
        {
            return L"mbfTwain-diagnostics.log";
        }

        if (length < buffer.size())
        {
            return std::wstring(buffer.data(), length) + L"mbfTwain-diagnostics.log";
        }

        buffer.resize(static_cast<size_t>(length) + 1U);
    }
}

inline std::string WideToUtf8(std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }

    const int length = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (length <= 0)
    {
        return {};
    }

    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        length,
        nullptr,
        nullptr);
    return result;
}

inline std::wstring FormatWindowsError(DWORD error)
{
    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    std::wstring message;
    if (length == 0 || buffer == nullptr)
    {
        message = L"unknown error";
    }
    else
    {
        message.assign(buffer, length);
        while (!message.empty() &&
               (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' '))
        {
            message.pop_back();
        }
    }

    if (buffer != nullptr)
    {
        LocalFree(buffer);
    }

    return message;
}

inline void AppendLine(std::wstring_view component, std::wstring_view message)
{
    static std::mutex mutex;

    SYSTEMTIME now{};
    GetLocalTime(&now);

    std::wostringstream line;
    line << now.wYear << L"-";
    if (now.wMonth < 10)
    {
        line << L"0";
    }
    line << now.wMonth << L"-";
    if (now.wDay < 10)
    {
        line << L"0";
    }
    line << now.wDay << L" ";
    if (now.wHour < 10)
    {
        line << L"0";
    }
    line << now.wHour << L":";
    if (now.wMinute < 10)
    {
        line << L"0";
    }
    line << now.wMinute << L":";
    if (now.wSecond < 10)
    {
        line << L"0";
    }
    line << now.wSecond << L".";
    if (now.wMilliseconds < 100)
    {
        line << L"0";
    }
    if (now.wMilliseconds < 10)
    {
        line << L"0";
    }
    line << now.wMilliseconds
         << L" pid=" << GetCurrentProcessId()
         << L" tid=" << GetCurrentThreadId()
         << L" [" << component << L"] "
         << message
         << L"\r\n";

    const std::string utf8 = WideToUtf8(line.str());
    if (utf8.empty())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex);
    const std::wstring path = LogFilePath();
    HANDLE file = CreateFileW(
        path.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }

    DWORD bytesWritten = 0;
    WriteFile(
        file,
        utf8.data(),
        static_cast<DWORD>(utf8.size()),
        &bytesWritten,
        nullptr);
    CloseHandle(file);
}

} // namespace mbf::twain::diagnostics
