#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ImageDib.h"
#include "ScannerIpcClient.h"
#include "twain.h"

namespace mbf::twain
{

class TransferSession final
{
public:
    void Begin(
        std::uint32_t revision,
        std::vector<ScannerIpcImage>&& images,
        std::uint32_t transferBufferDelayMilliseconds);
    void Clear() noexcept;
    void DiscardImages() noexcept;

    std::uint32_t Revision() const noexcept { return revision_; }
    bool HasRevision() const noexcept { return revision_ != 0; }
    std::uint32_t TakeRevision() noexcept;
    std::uint32_t TransferBufferDelayMilliseconds() const noexcept { return transferBufferDelayMilliseconds_; }

    size_t ImageCount() const noexcept { return images_.size(); }
    void LimitImages(size_t imageCount);
    bool HasPendingImage() const noexcept;
    TW_UINT32 PendingImageIndex() const noexcept { return pendingImageIndex_; }
    TW_UINT32 RemainingImageCount() const noexcept;
    TW_UINT16 RemainingImageCountForTwain() const noexcept;
    const ScannerIpcImage& ImageAt(TW_UINT32 imageIndex) const noexcept;
    const ScannerIpcImage& PendingImage() const noexcept;
    bool MatchesImage(TW_UINT32 imageIndex, const ScannerIpcImage& image) const noexcept;

    void AdvancePendingImage() noexcept;
    void ResetPendingImageIndex() noexcept;

    void MarkCurrentImage(TW_UINT32 imageIndex) noexcept;
    void ClearCurrentImage() noexcept;
    bool HasCurrentImage() const noexcept { return hasCurrentImage_; }
    TW_UINT32 CurrentImageIndex() const noexcept { return currentImageIndex_; }

    bool ScanUiHidden() const noexcept { return scanUiHidden_; }
    void MarkScanUiHidden() noexcept { scanUiHidden_ = true; }

    void ResetMemory() noexcept;
    bool HasMemoryForPendingImage() const noexcept;
    bool MemoryHasRowsRemaining() const noexcept;
    void BeginMemory(TW_UINT32 imageIndex, RasterImage&& image) noexcept;
    const RasterImage& MemoryImage() const noexcept { return memory_.image; }
    TW_UINT32 MemoryImageIndex() const noexcept { return memory_.imageIndex; }
    TW_UINT32 MemoryNextRow() const noexcept { return memory_.nextRow; }
    void AdvanceMemoryRows(TW_UINT32 rows) noexcept;
    bool MemoryComplete() const noexcept;

private:
    struct MemoryTransferState
    {
        bool active = false;
        TW_UINT32 imageIndex = 0;
        TW_UINT32 nextRow = 0;
        RasterImage image{};
    };

    std::uint32_t revision_ = 0;
    std::uint32_t transferBufferDelayMilliseconds_ = 100;
    TW_UINT32 pendingImageIndex_ = 0;
    bool hasCurrentImage_ = false;
    TW_UINT32 currentImageIndex_ = 0;
    bool scanUiHidden_ = false;
    std::vector<ScannerIpcImage> images_{};
    MemoryTransferState memory_{};
};

} // namespace mbf::twain
