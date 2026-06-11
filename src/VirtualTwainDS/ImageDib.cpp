#include "ImageDib.h"

#include <objbase.h>
#include <propidl.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{

template <typename T>
class ComPtr final
{
public:
    ~ComPtr()
    {
        Reset();
    }

    T** Put() noexcept
    {
        Reset();
        return &value_;
    }

    T* Get() const noexcept
    {
        return value_;
    }

    T* operator->() const noexcept
    {
        return value_;
    }

private:
    void Reset() noexcept
    {
        if (value_ != nullptr)
        {
            value_->Release();
            value_ = nullptr;
        }
    }

    T* value_ = nullptr;
};

class ComInitialization final
{
public:
    ComInitialization() noexcept
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        uninitialize_ = hr == S_OK || hr == S_FALSE;
        usable_ = uninitialize_ || hr == RPC_E_CHANGED_MODE;
    }

    ~ComInitialization()
    {
        if (uninitialize_)
        {
            CoUninitialize();
        }
    }

    bool Usable() const noexcept
    {
        return usable_;
    }

private:
    bool usable_ = false;
    bool uninitialize_ = false;
};

bool CreateWicFactory(ComPtr<IWICImagingFactory>& factory) noexcept
{
    return SUCCEEDED(CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.Put())));
}

bool CreateFrame(
    IWICImagingFactory* factory,
    const std::wstring& path,
    ComPtr<IWICBitmapFrameDecode>& frame) noexcept
{
    if (factory == nullptr || path.empty())
    {
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(
            path.c_str(),
            nullptr,
            GENERIC_READ,
            WICDecodeMetadataCacheOnDemand,
            decoder.Put())))
    {
        return false;
    }

    return SUCCEEDED(decoder->GetFrame(0, frame.Put()));
}

UINT ReadExifOrientation(IWICBitmapFrameDecode* frame) noexcept
{
    if (frame == nullptr)
    {
        return 1;
    }

    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(reader.Put())))
    {
        return 1;
    }

    const wchar_t* paths[] = {
        L"/app1/ifd/{ushort=274}",
        L"/ifd/{ushort=274}",
    };

    for (const wchar_t* path : paths)
    {
        PROPVARIANT value;
        PropVariantInit(&value);
        const HRESULT hr = reader->GetMetadataByName(path, &value);
        if (SUCCEEDED(hr))
        {
            UINT orientation = 1;
            switch (value.vt)
            {
            case VT_UI2:
                orientation = value.uiVal;
                break;
            case VT_UI4:
                orientation = value.ulVal;
                break;
            case VT_I4:
                orientation = value.lVal > 0 ? static_cast<UINT>(value.lVal) : 1;
                break;
            default:
                break;
            }

            PropVariantClear(&value);
            return orientation >= 1 && orientation <= 8 ? orientation : 1;
        }
        PropVariantClear(&value);
    }

    return 1;
}

bool ExifOrientationSwapsDimensions(UINT orientation) noexcept
{
    return orientation >= 5 && orientation <= 8;
}

bool DecodeBgra(
    IWICImagingFactory* factory,
    IWICBitmapSource* source,
    std::vector<BYTE>& pixels,
    UINT& width,
    UINT& height,
    UINT& stride) noexcept
{
    if (factory == nullptr || source == nullptr)
    {
        return false;
    }

    if (FAILED(source->GetSize(&width, &height)) || width == 0 || height == 0)
    {
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.Put())))
    {
        return false;
    }

    if (FAILED(converter->Initialize(
            source,
            GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom)))
    {
        return false;
    }

    stride = width * 4U;
    const auto imageBytes = static_cast<size_t>(stride) * static_cast<size_t>(height);
    pixels.assign(imageBytes, 0);

    return SUCCEEDED(converter->CopyPixels(
        nullptr,
        stride,
        static_cast<UINT>(pixels.size()),
        pixels.data()));
}

void MapExifOrientedPixel(
    UINT orientation,
    UINT destinationX,
    UINT destinationY,
    UINT sourceWidth,
    UINT sourceHeight,
    UINT& sourceX,
    UINT& sourceY) noexcept
{
    switch (orientation)
    {
    case 2:
        sourceX = sourceWidth - 1U - destinationX;
        sourceY = destinationY;
        break;
    case 3:
        sourceX = sourceWidth - 1U - destinationX;
        sourceY = sourceHeight - 1U - destinationY;
        break;
    case 4:
        sourceX = destinationX;
        sourceY = sourceHeight - 1U - destinationY;
        break;
    case 5:
        sourceX = destinationY;
        sourceY = destinationX;
        break;
    case 6:
        sourceX = destinationY;
        sourceY = sourceHeight - 1U - destinationX;
        break;
    case 7:
        sourceX = sourceWidth - 1U - destinationY;
        sourceY = sourceHeight - 1U - destinationX;
        break;
    case 8:
        sourceX = sourceWidth - 1U - destinationY;
        sourceY = destinationX;
        break;
    case 1:
    default:
        sourceX = destinationX;
        sourceY = destinationY;
        break;
    }
}

bool ApplyExifOrientationToBgra(
    std::vector<BYTE>& pixels,
    UINT& width,
    UINT& height,
    UINT& stride,
    UINT orientation) noexcept
{
    if (orientation <= 1 || orientation > 8)
    {
        return true;
    }

    if (width == 0 || height == 0 || stride < width * 4U ||
        pixels.size() < static_cast<size_t>(stride) * static_cast<size_t>(height))
    {
        return false;
    }

    const UINT sourceWidth = width;
    const UINT sourceHeight = height;
    const UINT sourceStride = stride;
    const UINT destinationWidth = ExifOrientationSwapsDimensions(orientation) ? sourceHeight : sourceWidth;
    const UINT destinationHeight = ExifOrientationSwapsDimensions(orientation) ? sourceWidth : sourceHeight;
    const UINT destinationStride = destinationWidth * 4U;
    std::vector<BYTE> oriented(
        static_cast<size_t>(destinationStride) * static_cast<size_t>(destinationHeight));

    for (UINT y = 0; y < destinationHeight; ++y)
    {
        BYTE* destinationRow = oriented.data() + (static_cast<size_t>(y) * destinationStride);
        for (UINT x = 0; x < destinationWidth; ++x)
        {
            UINT sourceX = 0;
            UINT sourceY = 0;
            MapExifOrientedPixel(orientation, x, y, sourceWidth, sourceHeight, sourceX, sourceY);
            const BYTE* sourcePixel = pixels.data() +
                (static_cast<size_t>(sourceY) * sourceStride) +
                (static_cast<size_t>(sourceX) * 4U);
            std::memcpy(destinationRow + (static_cast<size_t>(x) * 4U), sourcePixel, 4U);
        }
    }

    pixels.swap(oriented);
    width = destinationWidth;
    height = destinationHeight;
    stride = destinationStride;
    return true;
}

WORD BitsPerPixel(TW_UINT16 pixelType) noexcept
{
    switch (pixelType)
    {
    case TWPT_BW:
        return 1;
    case TWPT_GRAY:
        return 8;
    case TWPT_RGB:
    default:
        return 24;
    }
}

size_t PaletteEntries(TW_UINT16 pixelType) noexcept
{
    switch (pixelType)
    {
    case TWPT_BW:
        return 2;
    case TWPT_GRAY:
        return 256;
    default:
        return 0;
    }
}

size_t DibStride(UINT width, WORD bitsPerPixel) noexcept
{
    return ((static_cast<size_t>(width) * bitsPerPixel + 31U) / 32U) * 4U;
}

size_t RasterStride(UINT width, WORD bitsPerPixel) noexcept
{
    return (static_cast<size_t>(width) * bitsPerPixel + 7U) / 8U;
}

BYTE Luminance(BYTE red, BYTE green, BYTE blue) noexcept
{
    return static_cast<BYTE>((299U * red + 587U * green + 114U * blue) / 1000U);
}

void FillPalette(BITMAPINFOHEADER* header, TW_UINT16 pixelType) noexcept
{
    auto* palette = reinterpret_cast<RGBQUAD*>(reinterpret_cast<BYTE*>(header) + sizeof(BITMAPINFOHEADER));
    if (pixelType == TWPT_BW)
    {
        palette[0] = RGBQUAD{0, 0, 0, 0};
        palette[1] = RGBQUAD{255, 255, 255, 0};
        return;
    }

    if (pixelType == TWPT_GRAY)
    {
        for (size_t index = 0; index < 256; ++index)
        {
            const auto value = static_cast<BYTE>(index);
            palette[index] = RGBQUAD{value, value, value, 0};
        }
    }
}

void FillRgbBits(
    BYTE* destination,
    size_t destinationStride,
    const std::vector<BYTE>& source,
    UINT sourceStride,
    UINT width,
    UINT height) noexcept
{
    for (UINT y = 0; y < height; ++y)
    {
        const BYTE* sourceRow = source.data() + (static_cast<size_t>(y) * sourceStride);
        BYTE* destinationRow = destination + (static_cast<size_t>(height - 1U - y) * destinationStride);
        for (UINT x = 0; x < width; ++x)
        {
            destinationRow[x * 3U + 0U] = sourceRow[x * 4U + 0U];
            destinationRow[x * 3U + 1U] = sourceRow[x * 4U + 1U];
            destinationRow[x * 3U + 2U] = sourceRow[x * 4U + 2U];
        }
    }
}

void FillRgbRaster(
    BYTE* destination,
    size_t destinationStride,
    const std::vector<BYTE>& source,
    UINT sourceStride,
    UINT width,
    UINT height) noexcept
{
    for (UINT y = 0; y < height; ++y)
    {
        const BYTE* sourceRow = source.data() + (static_cast<size_t>(y) * sourceStride);
        BYTE* destinationRow = destination + (static_cast<size_t>(y) * destinationStride);
        for (UINT x = 0; x < width; ++x)
        {
            destinationRow[x * 3U + 0U] = sourceRow[x * 4U + 2U];
            destinationRow[x * 3U + 1U] = sourceRow[x * 4U + 1U];
            destinationRow[x * 3U + 2U] = sourceRow[x * 4U + 0U];
        }
    }
}

void FillGrayBits(
    BYTE* destination,
    size_t destinationStride,
    const std::vector<BYTE>& source,
    UINT sourceStride,
    UINT width,
    UINT height) noexcept
{
    for (UINT y = 0; y < height; ++y)
    {
        const BYTE* sourceRow = source.data() + (static_cast<size_t>(y) * sourceStride);
        BYTE* destinationRow = destination + (static_cast<size_t>(height - 1U - y) * destinationStride);
        for (UINT x = 0; x < width; ++x)
        {
            destinationRow[x] = Luminance(
                sourceRow[x * 4U + 2U],
                sourceRow[x * 4U + 1U],
                sourceRow[x * 4U + 0U]);
        }
    }
}

void FillGrayRaster(
    BYTE* destination,
    size_t destinationStride,
    const std::vector<BYTE>& source,
    UINT sourceStride,
    UINT width,
    UINT height) noexcept
{
    for (UINT y = 0; y < height; ++y)
    {
        const BYTE* sourceRow = source.data() + (static_cast<size_t>(y) * sourceStride);
        BYTE* destinationRow = destination + (static_cast<size_t>(y) * destinationStride);
        for (UINT x = 0; x < width; ++x)
        {
            destinationRow[x] = Luminance(
                sourceRow[x * 4U + 2U],
                sourceRow[x * 4U + 1U],
                sourceRow[x * 4U + 0U]);
        }
    }
}

void FillBlackWhiteBits(
    BYTE* destination,
    size_t destinationStride,
    const std::vector<BYTE>& source,
    UINT sourceStride,
    UINT width,
    UINT height) noexcept
{
    for (UINT y = 0; y < height; ++y)
    {
        const BYTE* sourceRow = source.data() + (static_cast<size_t>(y) * sourceStride);
        BYTE* destinationRow = destination + (static_cast<size_t>(height - 1U - y) * destinationStride);
        for (UINT x = 0; x < width; ++x)
        {
            const BYTE gray = Luminance(
                sourceRow[x * 4U + 2U],
                sourceRow[x * 4U + 1U],
                sourceRow[x * 4U + 0U]);
            if (gray >= 128)
            {
                destinationRow[x / 8U] |= static_cast<BYTE>(0x80U >> (x % 8U));
            }
        }
    }
}

void FillBlackWhiteRaster(
    BYTE* destination,
    size_t destinationStride,
    const std::vector<BYTE>& source,
    UINT sourceStride,
    UINT width,
    UINT height) noexcept
{
    for (UINT y = 0; y < height; ++y)
    {
        const BYTE* sourceRow = source.data() + (static_cast<size_t>(y) * sourceStride);
        BYTE* destinationRow = destination + (static_cast<size_t>(y) * destinationStride);
        for (UINT x = 0; x < width; ++x)
        {
            const BYTE gray = Luminance(
                sourceRow[x * 4U + 2U],
                sourceRow[x * 4U + 1U],
                sourceRow[x * 4U + 0U]);
            if (gray >= 128)
            {
                destinationRow[x / 8U] |= static_cast<BYTE>(0x80U >> (x % 8U));
            }
        }
    }
}

} // namespace

namespace mbf::twain
{

bool ImageDib::Probe(const std::wstring& path, DecodedImageInfo& info) noexcept
{
    const ComInitialization com;
    if (!com.Usable())
    {
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    if (!CreateWicFactory(factory))
    {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (!CreateFrame(factory.Get(), path, frame))
    {
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0)
    {
        return false;
    }

    if (ExifOrientationSwapsDimensions(ReadExifOrientation(frame.Get())))
    {
        std::swap(width, height);
    }

    info.width = width;
    info.height = height;
    return true;
}

bool ImageDib::BuildRaster(
    const std::wstring& path,
    TW_UINT16 pixelType,
    RasterImage& image) noexcept
{
    const ComInitialization com;
    if (!com.Usable())
    {
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    if (!CreateWicFactory(factory))
    {
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (!CreateFrame(factory.Get(), path, frame))
    {
        return false;
    }

    std::vector<BYTE> bgra;
    UINT width = 0;
    UINT height = 0;
    UINT sourceStride = 0;
    const UINT orientation = ReadExifOrientation(frame.Get());
    if (!DecodeBgra(factory.Get(), frame.Get(), bgra, width, height, sourceStride) ||
        !ApplyExifOrientationToBgra(bgra, width, height, sourceStride, orientation))
    {
        return false;
    }

    const WORD bitsPerPixel = BitsPerPixel(pixelType);
    const size_t destinationStride = RasterStride(width, bitsPerPixel);
    image.width = width;
    image.height = height;
    image.bitsPerPixel = bitsPerPixel;
    image.bytesPerRow = static_cast<TW_UINT32>(destinationStride);
    image.pixels.assign(destinationStride * height, 0);

    switch (pixelType)
    {
    case TWPT_BW:
        FillBlackWhiteRaster(image.pixels.data(), destinationStride, bgra, sourceStride, width, height);
        break;
    case TWPT_GRAY:
        FillGrayRaster(image.pixels.data(), destinationStride, bgra, sourceStride, width, height);
        break;
    case TWPT_RGB:
    default:
        FillRgbRaster(image.pixels.data(), destinationStride, bgra, sourceStride, width, height);
        break;
    }

    return true;
}

TW_HANDLE ImageDib::BuildNativeDib(
    const std::wstring& path,
    TW_UINT16 pixelType,
    DecodedImageInfo& info) noexcept
{
    const ComInitialization com;
    if (!com.Usable())
    {
        return nullptr;
    }

    ComPtr<IWICImagingFactory> factory;
    if (!CreateWicFactory(factory))
    {
        return nullptr;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    if (!CreateFrame(factory.Get(), path, frame))
    {
        return nullptr;
    }

    std::vector<BYTE> bgra;
    UINT width = 0;
    UINT height = 0;
    UINT sourceStride = 0;
    const UINT orientation = ReadExifOrientation(frame.Get());
    if (!DecodeBgra(factory.Get(), frame.Get(), bgra, width, height, sourceStride) ||
        !ApplyExifOrientationToBgra(bgra, width, height, sourceStride, orientation))
    {
        return nullptr;
    }

    const WORD bitsPerPixel = BitsPerPixel(pixelType);
    const size_t paletteBytes = PaletteEntries(pixelType) * sizeof(RGBQUAD);
    const size_t destinationStride = DibStride(width, bitsPerPixel);
    const size_t imageBytes = destinationStride * height;
    const size_t totalBytes = sizeof(BITMAPINFOHEADER) + paletteBytes + imageBytes;

    TW_HANDLE handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalBytes);
    if (handle == nullptr)
    {
        return nullptr;
    }

    auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(handle));
    if (header == nullptr)
    {
        GlobalFree(handle);
        return nullptr;
    }

    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = static_cast<LONG>(width);
    header->biHeight = static_cast<LONG>(height);
    header->biPlanes = 1;
    header->biBitCount = bitsPerPixel;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(imageBytes);
    header->biClrUsed = static_cast<DWORD>(PaletteEntries(pixelType));
    header->biClrImportant = header->biClrUsed;

    FillPalette(header, pixelType);

    BYTE* destination = reinterpret_cast<BYTE*>(header) + sizeof(BITMAPINFOHEADER) + paletteBytes;
    switch (pixelType)
    {
    case TWPT_BW:
        FillBlackWhiteBits(destination, destinationStride, bgra, sourceStride, width, height);
        break;
    case TWPT_GRAY:
        FillGrayBits(destination, destinationStride, bgra, sourceStride, width, height);
        break;
    case TWPT_RGB:
    default:
        FillRgbBits(destination, destinationStride, bgra, sourceStride, width, height);
        break;
    }

    GlobalUnlock(handle);

    info.width = width;
    info.height = height;
    return handle;
}

} // namespace mbf::twain
