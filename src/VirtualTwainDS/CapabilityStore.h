#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include "twain.h"

namespace mbf::twain
{
struct CapabilityResult
{
    TW_UINT16 returnCode;
    TW_UINT16 conditionCode;

    static constexpr CapabilityResult Success(TW_UINT16 conditionCode = TWCC_SUCCESS) noexcept
    {
        return {TWRC_SUCCESS, conditionCode};
    }

    static constexpr CapabilityResult Failure(TW_UINT16 conditionCode) noexcept
    {
        return {TWRC_FAILURE, conditionCode};
    }
};

class CapabilityStore final
{
public:
    CapabilityStore() noexcept;

    TW_BOOL DuplexEnabled() const noexcept { return settings_.duplexEnabled; }
    TW_UINT16 PixelType() const noexcept { return settings_.pixelType; }
    TW_UINT16 PaperSize() const noexcept { return settings_.paperSize; }
    TW_FIX32 XResolution() const noexcept { return settings_.xResolution; }
    TW_FIX32 YResolution() const noexcept { return settings_.yResolution; }
    TW_UINT16 TransferMechanism() const noexcept { return settings_.transferMechanism; }
    TW_INT16 TransferCount() const noexcept { return settings_.transferCount; }

    void SetDuplexEnabled(bool enabled) noexcept;
    bool SetPixelTypeIfSupported(TW_UINT16 pixelType) noexcept;
    bool SetPaperSizeIfSupported(TW_UINT16 paperSize) noexcept;
    bool SetXResolutionIfSupported(TW_FIX32 resolution) noexcept;
    bool SetYResolutionIfSupported(TW_FIX32 resolution) noexcept;

    CapabilityResult Get(TW_UINT16 message, pTW_CAPABILITY capability) const;
    CapabilityResult Set(pTW_CAPABILITY capability);
    CapabilityResult Reset(pTW_CAPABILITY capability);
    CapabilityResult QuerySupport(pTW_CAPABILITY capability) const;
    CapabilityResult ResetAll() noexcept;
    CapabilityResult ResetValue(TW_UINT16 capability) noexcept;

private:
    struct ScannerSettings
    {
        TW_BOOL duplexEnabled = FALSE;
        TW_UINT16 pixelType = TWPT_RGB;
        TW_UINT16 paperSize = TWSS_A4LETTER;
        TW_FIX32 xResolution{};
        TW_FIX32 yResolution{};
        TW_UINT16 transferMechanism = TWSX_NATIVE;
        TW_INT16 transferCount = -1;
    };

    ScannerSettings settings_{};
};

} // namespace mbf::twain
