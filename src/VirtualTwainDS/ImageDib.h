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

#include "twain.h"

namespace mbf::twain
{

struct DecodedImageInfo
{
    TW_UINT32 width = 0;
    TW_UINT32 height = 0;
};

struct RasterImage
{
    TW_UINT32 width = 0;
    TW_UINT32 height = 0;
    TW_UINT16 bitsPerPixel = 0;
    TW_UINT32 bytesPerRow = 0;
    std::vector<BYTE> pixels;
};

class ImageDib final
{
public:
    static bool Probe(
        const std::wstring& path,
        DecodedImageInfo& info,
        std::uint32_t rotationDegrees = 0) noexcept;
    static bool BuildRaster(
        const std::wstring& path,
        TW_UINT16 pixelType,
        RasterImage& image,
        std::uint32_t rotationDegrees = 0) noexcept;
    static TW_HANDLE BuildNativeDib(
        const std::wstring& path,
        TW_UINT16 pixelType,
        std::uint32_t xResolutionDpi,
        std::uint32_t yResolutionDpi,
        std::uint32_t rotationDegrees,
        DecodedImageInfo& info) noexcept;
};

} // namespace mbf::twain
