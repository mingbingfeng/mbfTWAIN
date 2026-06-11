#include "ScannerIpcClient.h"
#include "DiagnosticsLog.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <sstream>
#include <string_view>
#include <utility>

namespace
{

std::wstring Utf8ToWide(const std::string& value);

std::wstring CommandName(const std::string& command)
{
    const size_t end = command.find('\n');
    return Utf8ToWide(command.substr(0, end));
}

std::wstring ResponseSnippet(const std::string& response)
{
    constexpr size_t kMaxResponseBytes = 160;
    const std::string_view view(response.data(), (std::min)(response.size(), kMaxResponseBytes));
    return Utf8ToWide(std::string(view));
}

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }

    const int length = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (length <= 0)
    {
        return {};
    }

    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        length);
    return result;
}

bool ParseUInt32(std::string_view text, std::uint32_t& value) noexcept
{
    std::string owned(text);
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(owned.c_str(), &end, 10);
    if (errno != 0 || end == owned.c_str() || end == nullptr || *end != '\0')
    {
        return false;
    }

    value = static_cast<std::uint32_t>(parsed);
    return static_cast<unsigned long>(value) == parsed;
}

bool ParseBool(std::string_view text, bool& value) noexcept
{
    if (text == "1")
    {
        value = true;
        return true;
    }
    if (text == "0")
    {
        value = false;
        return true;
    }

    return false;
}

bool StartsWith(std::string_view text, std::string_view prefix) noexcept
{
    return text.size() >= prefix.size() &&
           text.substr(0, prefix.size()) == prefix;
}

bool ParseStateResponse(const std::string& response, mbf::twain::ScannerIpcState& state)
{
    std::istringstream stream(response);
    std::string line;

    if (!std::getline(stream, line))
    {
        return false;
    }
    if (!line.empty() && line.back() == '\r')
    {
        line.pop_back();
    }
    if (line != "OK STATE")
    {
        return false;
    }

    mbf::twain::ScannerIpcState parsed{};
    bool sawEnd = false;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        if (line == "END")
        {
            sawEnd = true;
            break;
        }

        const std::string_view kRevision("revision ", 9);
        const std::string_view kDuplex("duplex ", 7);
        const std::string_view kPixel("pixel ", 6);
        const std::string_view kXRes("xres ", 5);
        const std::string_view kYRes("yres ", 5);
        const std::string_view kScan("scan ", 5);
        const std::string_view kImage("image ", 6);

        std::uint32_t number = 0;
        bool boolean = false;
        if (StartsWith(line, kRevision) && ParseUInt32(std::string_view(line).substr(kRevision.size()), number))
        {
            parsed.revision = number;
        }
        else if (StartsWith(line, kDuplex) && ParseBool(std::string_view(line).substr(kDuplex.size()), boolean))
        {
            parsed.duplexEnabled = boolean;
        }
        else if (StartsWith(line, kPixel))
        {
            parsed.pixelType = Utf8ToWide(line.substr(kPixel.size()));
        }
        else if (StartsWith(line, kXRes) && ParseUInt32(std::string_view(line).substr(kXRes.size()), number))
        {
            parsed.xResolution = number;
        }
        else if (StartsWith(line, kYRes) && ParseUInt32(std::string_view(line).substr(kYRes.size()), number))
        {
            parsed.yResolution = number;
        }
        else if (StartsWith(line, kScan) && ParseBool(std::string_view(line).substr(kScan.size()), boolean))
        {
            parsed.scanRequested = boolean;
        }
        else if (StartsWith(line, kImage))
        {
            parsed.selectedImages.push_back(Utf8ToWide(line.substr(kImage.size())));
        }
    }

    if (!sawEnd)
    {
        return false;
    }

    state = std::move(parsed);
    return true;
}

} // namespace

namespace mbf::twain
{

ScannerIpcClient::ScannerIpcClient(std::wstring pipeName)
    : pipePath_(L"\\\\.\\pipe\\" + std::move(pipeName))
{
}

bool ScannerIpcClient::Ping(DWORD timeoutMilliseconds) const
{
    std::string response;
    return SendCommand("PING\n", response, timeoutMilliseconds) && response == "OK PONG\n";
}

bool ScannerIpcClient::BeginScan(DWORD timeoutMilliseconds) const
{
    std::string response;
    return SendCommand("BEGIN_SCAN\n", response, timeoutMilliseconds) &&
           response == "OK BEGIN_SCAN\n";
}

bool ScannerIpcClient::TryGetState(ScannerIpcState& state, DWORD timeoutMilliseconds) const
{
    std::string response;
    return SendCommand("GET_STATE\n", response, timeoutMilliseconds) &&
           ParseStateResponse(response, state);
}

bool ScannerIpcClient::AcknowledgeScan(std::uint32_t revision, DWORD timeoutMilliseconds) const
{
    std::string response;
    return SendCommand("ACK_SCAN " + std::to_string(revision) + "\n", response, timeoutMilliseconds) &&
           response == "OK ACK\n";
}

bool ScannerIpcClient::SendCommand(
    const std::string& command,
    std::string& response,
    DWORD timeoutMilliseconds) const
{
    response.clear();
    const std::wstring commandName = CommandName(command);
    diagnostics::AppendLine(
        L"IPC",
        L"SendCommand start pipe=" + pipePath_ +
            L" command=" + commandName +
            L" timeoutMs=" + std::to_wstring(timeoutMilliseconds));

    const ULONGLONG deadline = GetTickCount64() + timeoutMilliseconds;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (;;)
    {
        pipe = CreateFileW(
            pipePath_.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
        {
            break;
        }

        const DWORD error = GetLastError();
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
        {
            diagnostics::AppendLine(
                L"IPC",
                L"SendCommand timeout while opening pipe command=" + commandName +
                    L" lastError=" + std::to_wstring(error) +
                    L" (" + diagnostics::FormatWindowsError(error) + L")");
            return false;
        }

        const auto remaining = static_cast<DWORD>((std::min)(deadline - now, static_cast<ULONGLONG>(50)));
        if (error == ERROR_PIPE_BUSY)
        {
            diagnostics::AppendLine(
                L"IPC",
                L"Pipe busy for command=" + commandName +
                    L", waiting up to " + std::to_wstring(remaining) + L" ms");
            WaitNamedPipeW(pipePath_.c_str(), remaining);
        }
        else if (error == ERROR_FILE_NOT_FOUND)
        {
            Sleep((std::min)(remaining, static_cast<DWORD>(10)));
        }
        else
        {
            diagnostics::AppendLine(
                L"IPC",
                L"CreateFileW failed command=" + commandName +
                    L" error=" + std::to_wstring(error) +
                    L" (" + diagnostics::FormatWindowsError(error) + L")");
            return false;
        }
    }

    diagnostics::AppendLine(L"IPC", L"Connected to pipe for command=" + commandName);

    DWORD bytesWritten = 0;
    const auto bytesToWrite = static_cast<DWORD>(command.size());
    const BOOL writeOk = WriteFile(
        pipe,
        command.data(),
        bytesToWrite,
        &bytesWritten,
        nullptr);
    if (!writeOk || bytesWritten != bytesToWrite)
    {
        const DWORD error = writeOk ? ERROR_WRITE_FAULT : GetLastError();
        diagnostics::AppendLine(
            L"IPC",
            L"WriteFile failed command=" + commandName +
                L" bytesWritten=" + std::to_wstring(bytesWritten) +
                L" expected=" + std::to_wstring(bytesToWrite) +
                L" error=" + std::to_wstring(error) +
                L" (" + diagnostics::FormatWindowsError(error) + L")");
        CloseHandle(pipe);
        return false;
    }

    std::array<char, 1024> buffer{};
    for (;;)
    {
        DWORD bytesRead = 0;
        const BOOL readOk = ReadFile(
            pipe,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytesRead,
            nullptr);
        if (!readOk)
        {
            const DWORD error = GetLastError();
            CloseHandle(pipe);
            diagnostics::AppendLine(
                L"IPC",
                L"ReadFile finished command=" + commandName +
                    L" error=" + std::to_wstring(error) +
                    L" (" + diagnostics::FormatWindowsError(error) + L")" +
                    L" responseBytes=" + std::to_wstring(response.size()));
            return error == ERROR_BROKEN_PIPE && !response.empty();
        }
        if (bytesRead == 0)
        {
            break;
        }

        response.append(buffer.data(), bytesRead);
    }

    CloseHandle(pipe);
    diagnostics::AppendLine(
        L"IPC",
        L"SendCommand success command=" + commandName +
            L" responseBytes=" + std::to_wstring(response.size()) +
            L" responseSnippet=" + ResponseSnippet(response));
    return !response.empty();
}

} // namespace mbf::twain
