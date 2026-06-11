#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace mbf::twain
{

struct ScannerIpcState
{
    std::uint32_t revision = 0;
    bool duplexEnabled = false;
    std::wstring pixelType = L"RGB";
    std::wstring paperSize = L"A4";
    std::uint32_t xResolution = 300;
    std::uint32_t yResolution = 300;
    bool scanRequested = false;
    std::vector<std::wstring> selectedImages;
};

class ScannerIpcClient final
{
public:
    explicit ScannerIpcClient(std::wstring pipeName = L"mbfTwain.VirtualScanner.v1");

    bool Ping(DWORD timeoutMilliseconds = 150) const;
    bool BeginScan(DWORD timeoutMilliseconds = 150) const;
    bool BeginScan(const ScannerIpcState& initialState, DWORD timeoutMilliseconds = 150) const;
    bool TryGetState(ScannerIpcState& state, DWORD timeoutMilliseconds = 150) const;
    bool AcknowledgeScan(std::uint32_t revision, DWORD timeoutMilliseconds = 150) const;

private:
    bool SendCommand(
        const std::string& command,
        std::string& response,
        DWORD timeoutMilliseconds) const;

    std::wstring pipePath_;
};

} // namespace mbf::twain
