#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "ImageDib.h"
#include "twain.h"

namespace mbf::twain
{

enum class TwainState : TW_UINT16
{
    SourceLoaded = 3,
    SourceOpened = 4,
    SourceEnabled = 5,
    TransferReady = 6,
    Transferring = 7,
};

class VirtualTwainDataSource final
{
public:
    static VirtualTwainDataSource& Instance();

    TW_UINT16 Entry(
        pTW_IDENTITY origin,
        TW_UINT32 dataGroup,
        TW_UINT16 dataArgumentType,
        TW_UINT16 message,
        TW_MEMREF data);

private:
    struct ScannerSettings
    {
        TW_BOOL duplexEnabled = FALSE;
        TW_UINT16 pixelType = TWPT_RGB;
        TW_FIX32 xResolution{};
        TW_FIX32 yResolution{};
        TW_UINT16 transferMechanism = TWSX_NATIVE;
        TW_INT16 transferCount = -1;
    };

    struct MemoryTransferState
    {
        bool active = false;
        TW_UINT32 imageIndex = 0;
        TW_UINT32 nextRow = 0;
        RasterImage image{};
    };

    VirtualTwainDataSource();

    TW_UINT16 HandleIdentity(TW_UINT16 message, pTW_IDENTITY origin, TW_MEMREF data);
    TW_UINT16 HandleCapability(TW_UINT16 message, TW_MEMREF data);
    TW_UINT16 HandleUserInterface(TW_UINT16 message, TW_MEMREF data);
    TW_UINT16 HandleEvent(TW_UINT16 message, TW_MEMREF data);
    TW_UINT16 HandlePendingTransfers(TW_UINT16 message, TW_MEMREF data);
    TW_UINT16 HandleSetupMemoryTransfer(TW_UINT16 message, TW_MEMREF data);
    TW_UINT16 HandleImage(TW_UINT16 dataArgumentType, TW_UINT16 message, TW_MEMREF data);
    TW_UINT16 HandleImageInfo(TW_UINT16 message, TW_MEMREF data);
    TW_UINT16 HandleImageNativeTransfer(TW_UINT16 message, TW_MEMREF data);
    TW_UINT16 HandleImageMemoryTransfer(TW_UINT16 message, TW_MEMREF data);
    TW_UINT16 HandleStatus(TW_UINT16 message, TW_MEMREF data);

    TW_UINT16 GetCapability(TW_UINT16 message, pTW_CAPABILITY capability);
    TW_UINT16 SetCapability(pTW_CAPABILITY capability);
    TW_UINT16 ResetCapability(pTW_CAPABILITY capability);
    TW_UINT16 QueryCapabilitySupport(pTW_CAPABILITY capability);
    TW_UINT16 ResetAllCapabilities() noexcept;
    TW_UINT16 ResetCapabilityValue(TW_UINT16 capability) noexcept;
    bool RefreshTransferReadyFromIpc();
    bool BeginUiScanSession(bool shouldShowUi);
    bool LaunchScannerUiProcess() const;
    bool FillCurrentImageInfo(pTW_IMAGEINFO imageInfo);
    bool EnsureMemoryTransferReady();
    void ResetMemoryTransfer() noexcept;
    void AcknowledgeScanIfComplete();

    TW_UINT16 Succeed(TW_UINT16 conditionCode = TWCC_SUCCESS) noexcept;
    TW_UINT16 Fail(TW_UINT16 conditionCode) noexcept;
    bool IsAtLeast(TwainState state) const noexcept;

    static TW_IDENTITY BuildSourceIdentity() noexcept;
    static TW_FIX32 MakeFix32(TW_INT16 whole, TW_UINT16 fraction = 0) noexcept;

    mutable std::mutex mutex_;
    TwainState state_ = TwainState::SourceLoaded;
    TW_IDENTITY identity_{};
    TW_STATUS lastStatus_{};
    ScannerSettings settings_{};
    bool transferReady_ = false;
    std::uint32_t pendingIpcRevision_ = 0;
    TW_UINT32 pendingTransferIndex_ = 0;
    std::vector<std::wstring> pendingImages_;
    MemoryTransferState memoryTransfer_{};
    std::optional<TW_IDENTITY> openOrigin_;
};

} // namespace mbf::twain
