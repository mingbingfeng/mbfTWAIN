#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "CapabilityStore.h"
#include "ImageDib.h"
#include "IpcSession.h"
#include "ScannerIpcClient.h"
#include "TransferSession.h"
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
    VirtualTwainDataSource();
    ~VirtualTwainDataSource();

    TW_UINT16 HandleIdentity(
        TW_UINT16 message,
        pTW_IDENTITY origin,
        TW_MEMREF data,
        std::unique_lock<std::mutex>& lock);
    TW_UINT16 HandleCapability(TW_UINT16 message, TW_MEMREF data);
    TW_UINT16 HandleUserInterface(TW_UINT16 message, TW_MEMREF data, std::unique_lock<std::mutex>& lock);
    TW_UINT16 HandleEvent(TW_UINT16 message, TW_MEMREF data, std::unique_lock<std::mutex>& lock);
    TW_UINT16 HandlePendingTransfers(TW_UINT16 message, TW_MEMREF data, std::unique_lock<std::mutex>& lock);
    TW_UINT16 HandleSetupMemoryTransfer(TW_UINT16 message, TW_MEMREF data);
    TW_UINT16 HandleTransferGroup(TW_UINT16 message, TW_MEMREF data);
    TW_UINT16 HandleEntryPoint(TW_UINT16 message, TW_MEMREF data);
    TW_UINT16 HandleImage(
        TW_UINT16 dataArgumentType,
        TW_UINT16 message,
        TW_MEMREF data,
        std::unique_lock<std::mutex>& lock);
    TW_UINT16 HandleImageInfo(TW_UINT16 message, TW_MEMREF data, std::unique_lock<std::mutex>& lock);
    TW_UINT16 HandleImageNativeTransfer(TW_UINT16 message, TW_MEMREF data, std::unique_lock<std::mutex>& lock);
    TW_UINT16 HandleImageMemoryTransfer(TW_UINT16 message, TW_MEMREF data, std::unique_lock<std::mutex>& lock);
    TW_UINT16 HandleStatus(TW_UINT16 message, TW_MEMREF data);

    TW_UINT16 GetCapability(TW_UINT16 message, pTW_CAPABILITY capability);
    TW_UINT16 SetCapability(pTW_CAPABILITY capability);
    TW_UINT16 ResetCapability(pTW_CAPABILITY capability);
    TW_UINT16 QueryCapabilitySupport(pTW_CAPABILITY capability);
    TW_UINT16 ResetAllCapabilities() noexcept;
    TW_UINT16 ResetCapabilityValue(TW_UINT16 capability) noexcept;
    bool RefreshTransferReadyFromIpc(std::unique_lock<std::mutex>& lock);
    void ApplyScannerSettingsFromIpc(const ScannerIpcState& ipcState);
    void CommitTransferReadyFromIpc(ScannerIpcState&& ipcState);
    bool NotifyTransferReady(std::unique_lock<std::mutex>& lock);
    void StartTransferReadyWatcher();
    void StopTransferReadyWatcher();
    void TransferReadyWatcherLoop();
    bool BeginUiScanSession(const ScannerIpcState& initialState);
    bool LaunchScannerUiProcess() const;
    bool FillImageInfo(
        TW_UINT32 imageIndex,
        pTW_IMAGEINFO imageInfo,
        std::unique_lock<std::mutex>& lock);
    bool TryResolveImageInfoIndex(TW_UINT32& imageIndex) const noexcept;
    bool EnsureMemoryTransferReady(std::unique_lock<std::mutex>& lock);
    void ResetMemoryTransfer() noexcept;
    void HideScanUiSession(std::unique_lock<std::mutex>& lock);
    void HideScanUiIfTransferStarted(std::unique_lock<std::mutex>& lock);
    void ClearTransferProgress() noexcept;
    void AcknowledgeScanIfComplete(std::unique_lock<std::mutex>& lock);
    void RollbackCanceledUiEnable();

    TW_UINT16 Succeed(TW_UINT16 conditionCode = TWCC_SUCCESS) noexcept;
    TW_UINT16 Fail(TW_UINT16 conditionCode) noexcept;
    TW_UINT16 CompleteCapabilityResult(CapabilityResult result) noexcept;
    bool IsAtLeast(TwainState state) const noexcept;

    static TW_IDENTITY BuildSourceIdentity() noexcept;
    static TW_FIX32 MakeFix32(TW_INT16 whole, TW_UINT16 fraction = 0) noexcept;

    mutable std::mutex mutex_;
    TwainState state_ = TwainState::SourceLoaded;
    TW_IDENTITY identity_{};
    TW_STATUS lastStatus_{};
    CapabilityStore capabilities_{};
    bool transferReady_ = false;
    bool transferReadyNotified_ = false;
    IpcSession ipcSession_{};
    TransferSession transferSession_{};
    TW_ENTRYPOINT entryPoint_{};
    std::optional<TW_IDENTITY> openOrigin_;
    std::condition_variable transferReadyWatcherCv_;
    std::thread transferReadyWatcher_;
    bool transferReadyWatcherStop_ = false;
    bool transferReadyWatcherActive_ = false;
    std::uint64_t transferReadyWatcherGeneration_ = 0;
};

} // namespace mbf::twain
