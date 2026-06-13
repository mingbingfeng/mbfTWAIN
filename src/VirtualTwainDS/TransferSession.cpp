#include "TransferSession.h"

#include <algorithm>
#include <utility>

namespace mbf::twain
{

void TransferSession::Begin(std::uint32_t revision, std::vector<ScannerIpcImage>&& images)
{
    revision_ = revision;
    pendingImageIndex_ = 0;
    hasCurrentImage_ = false;
    currentImageIndex_ = 0;
    scanUiHidden_ = false;
    ResetMemory();
    images_ = std::move(images);
}

void TransferSession::Clear() noexcept
{
    images_.clear();
    revision_ = 0;
    pendingImageIndex_ = 0;
    hasCurrentImage_ = false;
    currentImageIndex_ = 0;
    scanUiHidden_ = false;
    ResetMemory();
}

void TransferSession::DiscardImages() noexcept
{
    images_.clear();
    pendingImageIndex_ = 0;
    hasCurrentImage_ = false;
    currentImageIndex_ = 0;
    scanUiHidden_ = false;
    ResetMemory();
}

std::uint32_t TransferSession::TakeRevision() noexcept
{
    const std::uint32_t revision = revision_;
    revision_ = 0;
    return revision;
}

bool TransferSession::HasPendingImage() const noexcept
{
    return pendingImageIndex_ < images_.size();
}

void TransferSession::LimitImages(size_t imageCount)
{
    if (images_.size() > imageCount)
    {
        images_.resize(imageCount);
        if (pendingImageIndex_ > images_.size())
        {
            pendingImageIndex_ = static_cast<TW_UINT32>(images_.size());
        }
        if (hasCurrentImage_ && currentImageIndex_ >= images_.size())
        {
            ClearCurrentImage();
        }
        if (memory_.active && memory_.imageIndex >= images_.size())
        {
            ResetMemory();
        }
    }
}

TW_UINT32 TransferSession::RemainingImageCount() const noexcept
{
    return HasPendingImage()
        ? static_cast<TW_UINT32>(images_.size() - pendingImageIndex_)
        : 0;
}

TW_UINT16 TransferSession::RemainingImageCountForTwain() const noexcept
{
    return static_cast<TW_UINT16>((std::min)(RemainingImageCount(), static_cast<TW_UINT32>(0xffff)));
}

const ScannerIpcImage& TransferSession::ImageAt(TW_UINT32 imageIndex) const noexcept
{
    return images_[imageIndex];
}

const ScannerIpcImage& TransferSession::PendingImage() const noexcept
{
    return ImageAt(pendingImageIndex_);
}

bool TransferSession::MatchesImage(TW_UINT32 imageIndex, const ScannerIpcImage& image) const noexcept
{
    return imageIndex < images_.size() &&
        images_[imageIndex].path == image.path &&
        images_[imageIndex].rotationDegrees == image.rotationDegrees;
}

void TransferSession::AdvancePendingImage() noexcept
{
    ++pendingImageIndex_;
}

void TransferSession::ResetPendingImageIndex() noexcept
{
    pendingImageIndex_ = 0;
}

void TransferSession::MarkCurrentImage(TW_UINT32 imageIndex) noexcept
{
    currentImageIndex_ = imageIndex;
    hasCurrentImage_ = true;
}

void TransferSession::ClearCurrentImage() noexcept
{
    currentImageIndex_ = 0;
    hasCurrentImage_ = false;
}

void TransferSession::ResetMemory() noexcept
{
    memory_.active = false;
    memory_.imageIndex = 0;
    memory_.nextRow = 0;
    memory_.image = RasterImage{};
}

bool TransferSession::HasMemoryForPendingImage() const noexcept
{
    return memory_.active && memory_.imageIndex == pendingImageIndex_;
}

bool TransferSession::MemoryHasRowsRemaining() const noexcept
{
    return memory_.nextRow < memory_.image.height;
}

void TransferSession::BeginMemory(TW_UINT32 imageIndex, RasterImage&& image) noexcept
{
    memory_.active = true;
    memory_.imageIndex = imageIndex;
    memory_.nextRow = 0;
    memory_.image = std::move(image);
}

void TransferSession::AdvanceMemoryRows(TW_UINT32 rows) noexcept
{
    memory_.nextRow += rows;
}

bool TransferSession::MemoryComplete() const noexcept
{
    return memory_.nextRow >= memory_.image.height;
}

} // namespace mbf::twain
