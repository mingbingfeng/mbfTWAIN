#include "CapabilityStore.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

namespace
{

constexpr std::array<TW_UINT16, 14> kSupportedCapabilities = {
    CAP_XFERCOUNT,
    CAP_SUPPORTEDCAPS,
    CAP_SUPPORTEDDATS,
    CAP_FEEDERENABLED,
    CAP_UICONTROLLABLE,
    CAP_DUPLEX,
    CAP_DUPLEXENABLED,
    ICAP_PIXELTYPE,
    ICAP_XRESOLUTION,
    ICAP_YRESOLUTION,
    ICAP_SUPPORTEDSIZES,
    ICAP_AUTOMATICBORDERDETECTION,
    ICAP_AUTOMATICDESKEW,
    ICAP_XFERMECH,
};

constexpr std::array<TW_UINT32, 14> kSupportedDataArgumentTypes = {
    (DG_CONTROL << 16) | DAT_CAPABILITY,
    (DG_CONTROL << 16) | DAT_ENTRYPOINT,
    (DG_CONTROL << 16) | DAT_EVENT,
    (DG_CONTROL << 16) | DAT_IDENTITY,
    (DG_CONTROL << 16) | DAT_PENDINGXFERS,
    (DG_CONTROL << 16) | DAT_SETUPMEMXFER,
    (DG_CONTROL << 16) | DAT_STATUS,
    (DG_CONTROL << 16) | DAT_USERINTERFACE,
    (DG_CONTROL << 16) | DAT_XFERGROUP,
    (DG_IMAGE << 16) | DAT_IMAGEINFO,
    (DG_IMAGE << 16) | DAT_IMAGEMEMXFER,
    (DG_IMAGE << 16) | DAT_IMAGENATIVEXFER,
    (DG_CONTROL << 16) | DAT_CALLBACK,
    (DG_CONTROL << 16) | DAT_CALLBACK2,
};

constexpr std::array<TW_BOOL, 2> kDuplexValues = {FALSE, TRUE};
constexpr std::array<TW_BOOL, 1> kTrueOnlyBoolValues = {TRUE};
constexpr std::array<TW_UINT16, 3> kPixelTypeValues = {TWPT_BW, TWPT_GRAY, TWPT_RGB};
constexpr std::array<TW_UINT16, 2> kTransferMechanismValues = {TWSX_NATIVE, TWSX_MEMORY};
constexpr std::array<TW_UINT16, 2> kSupportedSizesValues = {TWSS_A4LETTER, TWSS_A3};
constexpr std::array<TW_INT16, 4> kResolutionWholeValues = {150, 200, 300, 600};

constexpr TW_UINT32 kMutableCapabilitySupport =
    TWQC_GET | TWQC_SET | TWQC_GETDEFAULT | TWQC_GETCURRENT | TWQC_RESET;
constexpr TW_UINT32 kReadOnlyCapabilitySupport =
    TWQC_GET | TWQC_GETDEFAULT | TWQC_GETCURRENT;

TW_UINT32 PackFix32(const TW_FIX32& value) noexcept
{
    TW_UINT32 packed = 0;
    static_assert(sizeof(packed) == sizeof(value), "TW_FIX32 must fit TW_ONEVALUE::Item");
    std::memcpy(&packed, &value, sizeof(packed));
    return packed;
}

TW_FIX32 UnpackFix32(TW_UINT32 packed) noexcept
{
    TW_FIX32 value{};
    static_assert(sizeof(packed) == sizeof(value), "TW_FIX32 must fit TW_ONEVALUE::Item");
    std::memcpy(&value, &packed, sizeof(value));
    return value;
}

TW_FIX32 MakeFix32(TW_INT16 whole, TW_UINT16 fraction = 0) noexcept
{
    TW_FIX32 value{};
    value.Whole = whole;
    value.Frac = fraction;
    return value;
}

size_t ItemSize(TW_UINT16 itemType) noexcept
{
    switch (itemType)
    {
    case TWTY_BOOL:
        return sizeof(TW_BOOL);
    case TWTY_INT16:
        return sizeof(TW_INT16);
    case TWTY_UINT16:
        return sizeof(TW_UINT16);
    case TWTY_INT32:
        return sizeof(TW_INT32);
    case TWTY_UINT32:
        return sizeof(TW_UINT32);
    case TWTY_FIX32:
        return sizeof(TW_FIX32);
    default:
        return 0;
    }
}

TW_HANDLE AllocateContainer(size_t bytes) noexcept
{
    return GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
}

void FreeContainer(TW_HANDLE handle) noexcept
{
    if (handle != nullptr)
    {
        GlobalFree(handle);
    }
}

TW_HANDLE BuildOneValue(TW_UINT16 itemType, TW_UINT32 packedItem) noexcept
{
    TW_HANDLE handle = AllocateContainer(sizeof(TW_ONEVALUE));
    if (handle == nullptr)
    {
        return nullptr;
    }

    auto* oneValue = static_cast<pTW_ONEVALUE>(GlobalLock(handle));
    if (oneValue == nullptr)
    {
        FreeContainer(handle);
        return nullptr;
    }

    oneValue->ItemType = itemType;
    oneValue->Item = packedItem;
    GlobalUnlock(handle);
    return handle;
}

TW_HANDLE BuildArray(TW_UINT16 itemType, const void* values, TW_UINT32 itemCount) noexcept
{
    const size_t itemSize = ItemSize(itemType);
    if (values == nullptr || itemSize == 0 || itemCount == 0)
    {
        return nullptr;
    }

    const size_t bytes = offsetof(TW_ARRAY, ItemList) + (itemSize * itemCount);
    TW_HANDLE handle = AllocateContainer(bytes);
    if (handle == nullptr)
    {
        return nullptr;
    }

    auto* array = static_cast<pTW_ARRAY>(GlobalLock(handle));
    if (array == nullptr)
    {
        FreeContainer(handle);
        return nullptr;
    }

    array->ItemType = itemType;
    array->NumItems = itemCount;
    std::memcpy(array->ItemList, values, itemSize * itemCount);
    GlobalUnlock(handle);
    return handle;
}

TW_HANDLE BuildEnumeration(
    TW_UINT16 itemType,
    const void* values,
    TW_UINT32 itemCount,
    TW_UINT32 currentIndex,
    TW_UINT32 defaultIndex) noexcept
{
    const size_t itemSize = ItemSize(itemType);
    if (values == nullptr || itemSize == 0 || itemCount == 0 ||
        currentIndex >= itemCount || defaultIndex >= itemCount)
    {
        return nullptr;
    }

    const size_t bytes = offsetof(TW_ENUMERATION, ItemList) + (itemSize * itemCount);
    TW_HANDLE handle = AllocateContainer(bytes);
    if (handle == nullptr)
    {
        return nullptr;
    }

    auto* enumeration = static_cast<pTW_ENUMERATION>(GlobalLock(handle));
    if (enumeration == nullptr)
    {
        FreeContainer(handle);
        return nullptr;
    }

    enumeration->ItemType = itemType;
    enumeration->NumItems = itemCount;
    enumeration->CurrentIndex = currentIndex;
    enumeration->DefaultIndex = defaultIndex;
    std::memcpy(enumeration->ItemList, values, itemSize * itemCount);
    GlobalUnlock(handle);
    return handle;
}

template <typename T, size_t N>
TW_UINT32 IndexOf(const std::array<T, N>& values, T value, TW_UINT32 fallback) noexcept
{
    for (TW_UINT32 index = 0; index < values.size(); ++index)
    {
        if (values[index] == value)
        {
            return index;
        }
    }

    return fallback;
}

std::array<TW_FIX32, kResolutionWholeValues.size()> ResolutionValues() noexcept
{
    std::array<TW_FIX32, kResolutionWholeValues.size()> values{};
    for (size_t index = 0; index < kResolutionWholeValues.size(); ++index)
    {
        values[index].Whole = kResolutionWholeValues[index];
        values[index].Frac = 0;
    }

    return values;
}

TW_UINT32 IndexOfResolution(TW_FIX32 value, TW_UINT32 fallback) noexcept
{
    if (value.Frac != 0)
    {
        return fallback;
    }

    for (TW_UINT32 index = 0; index < kResolutionWholeValues.size(); ++index)
    {
        if (kResolutionWholeValues[index] == value.Whole)
        {
            return index;
        }
    }

    return fallback;
}

bool ContainsFix32WholeValue(TW_FIX32 value) noexcept
{
    return IndexOfResolution(value, static_cast<TW_UINT32>(kResolutionWholeValues.size())) <
           kResolutionWholeValues.size();
}

template <typename T, size_t N>
bool Contains(const std::array<T, N>& values, T value) noexcept
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool ReadOneValue(TW_HANDLE handle, TW_UINT16 expectedItemType, TW_UINT32& packedItem) noexcept
{
    if (handle == nullptr)
    {
        return false;
    }

    auto* oneValue = static_cast<pTW_ONEVALUE>(GlobalLock(handle));
    if (oneValue == nullptr)
    {
        return false;
    }

    const bool ok = oneValue->ItemType == expectedItemType;
    if (ok)
    {
        packedItem = oneValue->Item;
    }

    GlobalUnlock(handle);
    return ok;
}

bool ReadEnumerationCurrent(TW_HANDLE handle, TW_UINT16 expectedItemType, TW_UINT32& packedItem) noexcept
{
    if (handle == nullptr)
    {
        return false;
    }

    auto* enumeration = static_cast<pTW_ENUMERATION>(GlobalLock(handle));
    if (enumeration == nullptr)
    {
        return false;
    }

    const size_t itemSize = ItemSize(expectedItemType);
    const bool ok =
        itemSize != 0 &&
        enumeration->ItemType == expectedItemType &&
        enumeration->CurrentIndex < enumeration->NumItems;
    if (ok)
    {
        packedItem = 0;
        const auto* item = static_cast<const BYTE*>(enumeration->ItemList) +
            (itemSize * enumeration->CurrentIndex);
        std::memcpy(&packedItem, item, itemSize);
    }

    GlobalUnlock(handle);
    return ok;
}

bool ReadCapabilityPackedItem(
    const TW_CAPABILITY& capability,
    TW_UINT16 expectedItemType,
    TW_UINT32& packedItem) noexcept
{
    switch (capability.ConType)
    {
    case TWON_ONEVALUE:
        return ReadOneValue(capability.hContainer, expectedItemType, packedItem);
    case TWON_ENUMERATION:
        return ReadEnumerationCurrent(capability.hContainer, expectedItemType, packedItem);
    default:
        return false;
    }
}

} // namespace

namespace mbf::twain
{

CapabilityStore::CapabilityStore() noexcept
{
    ResetAll();
}

void CapabilityStore::SetDuplexEnabled(bool enabled) noexcept
{
    settings_.duplexEnabled = enabled ? TRUE : FALSE;
}

bool CapabilityStore::SetPixelTypeIfSupported(TW_UINT16 pixelType) noexcept
{
    if (!Contains(kPixelTypeValues, pixelType))
    {
        return false;
    }

    settings_.pixelType = pixelType;
    return true;
}

bool CapabilityStore::SetPaperSizeIfSupported(TW_UINT16 paperSize) noexcept
{
    if (!Contains(kSupportedSizesValues, paperSize))
    {
        return false;
    }

    settings_.paperSize = paperSize;
    return true;
}

bool CapabilityStore::SetXResolutionIfSupported(TW_FIX32 resolution) noexcept
{
    if (!ContainsFix32WholeValue(resolution))
    {
        return false;
    }

    settings_.xResolution = resolution;
    return true;
}

bool CapabilityStore::SetYResolutionIfSupported(TW_FIX32 resolution) noexcept
{
    if (!ContainsFix32WholeValue(resolution))
    {
        return false;
    }

    settings_.yResolution = resolution;
    return true;
}

CapabilityResult CapabilityStore::Get(TW_UINT16 message, pTW_CAPABILITY capability) const
{
    TW_HANDLE container = nullptr;
    TW_UINT16 containerType = TWON_ONEVALUE;

    switch (capability->Cap)
    {
    case CAP_XFERCOUNT:
        container = BuildOneValue(TWTY_INT16, static_cast<TW_UINT16>(settings_.transferCount));
        break;

    case CAP_SUPPORTEDCAPS:
        containerType = TWON_ENUMERATION;
        container = BuildEnumeration(
            TWTY_UINT16,
            kSupportedCapabilities.data(),
            static_cast<TW_UINT32>(kSupportedCapabilities.size()),
            0,
            0);
        break;

    case CAP_SUPPORTEDDATS:
        containerType = TWON_ARRAY;
        container = BuildArray(
            TWTY_UINT32,
            kSupportedDataArgumentTypes.data(),
            static_cast<TW_UINT32>(kSupportedDataArgumentTypes.size()));
        break;

    case CAP_FEEDERENABLED:
    case CAP_UICONTROLLABLE:
    case CAP_DUPLEX:
        container = BuildOneValue(TWTY_BOOL, TRUE);
        break;

    case CAP_DUPLEXENABLED:
        if (message == MSG_GET)
        {
            containerType = TWON_ENUMERATION;
            container = BuildEnumeration(
                TWTY_BOOL,
                kDuplexValues.data(),
                static_cast<TW_UINT32>(kDuplexValues.size()),
                IndexOf(kDuplexValues, settings_.duplexEnabled, 0),
                0);
        }
        else
        {
            const TW_BOOL value = message == MSG_GETDEFAULT ? FALSE : settings_.duplexEnabled;
            container = BuildOneValue(TWTY_BOOL, value);
        }
        break;

    case ICAP_PIXELTYPE:
        if (message == MSG_GET)
        {
            containerType = TWON_ENUMERATION;
            container = BuildEnumeration(
                TWTY_UINT16,
                kPixelTypeValues.data(),
                static_cast<TW_UINT32>(kPixelTypeValues.size()),
                IndexOf(kPixelTypeValues, settings_.pixelType, 2),
                2);
        }
        else
        {
            const TW_UINT16 value = message == MSG_GETDEFAULT ? TWPT_RGB : settings_.pixelType;
            container = BuildOneValue(TWTY_UINT16, value);
        }
        break;

    case ICAP_XRESOLUTION:
    case ICAP_YRESOLUTION:
    {
        const TW_FIX32 currentValue =
            capability->Cap == ICAP_XRESOLUTION ? settings_.xResolution : settings_.yResolution;
        if (message == MSG_GET)
        {
            const auto values = ResolutionValues();
            containerType = TWON_ENUMERATION;
            container = BuildEnumeration(
                TWTY_FIX32,
                values.data(),
                static_cast<TW_UINT32>(values.size()),
                IndexOfResolution(currentValue, 2),
                2);
        }
        else
        {
            const TW_FIX32 value = message == MSG_GETDEFAULT ? MakeFix32(300) : currentValue;
            container = BuildOneValue(TWTY_FIX32, PackFix32(value));
        }
        break;
    }

    case ICAP_SUPPORTEDSIZES:
        if (message == MSG_GET)
        {
            containerType = TWON_ENUMERATION;
            container = BuildEnumeration(
                TWTY_UINT16,
                kSupportedSizesValues.data(),
                static_cast<TW_UINT32>(kSupportedSizesValues.size()),
                IndexOf(kSupportedSizesValues, settings_.paperSize, 0),
                0);
        }
        else
        {
            const TW_UINT16 value = message == MSG_GETDEFAULT ? TWSS_A4LETTER : settings_.paperSize;
            container = BuildOneValue(TWTY_UINT16, value);
        }
        break;

    case ICAP_AUTOMATICBORDERDETECTION:
    case ICAP_AUTOMATICDESKEW:
        if (message == MSG_GET)
        {
            containerType = TWON_ENUMERATION;
            container = BuildEnumeration(
                TWTY_BOOL,
                kTrueOnlyBoolValues.data(),
                static_cast<TW_UINT32>(kTrueOnlyBoolValues.size()),
                0,
                0);
        }
        else
        {
            container = BuildOneValue(TWTY_BOOL, TRUE);
        }
        break;

    case ICAP_XFERMECH:
        if (message == MSG_GET)
        {
            containerType = TWON_ENUMERATION;
            container = BuildEnumeration(
                TWTY_UINT16,
                kTransferMechanismValues.data(),
                static_cast<TW_UINT32>(kTransferMechanismValues.size()),
                IndexOf(kTransferMechanismValues, settings_.transferMechanism, 0),
                0);
        }
        else
        {
            const TW_UINT16 value = message == MSG_GETDEFAULT ? TWSX_NATIVE : settings_.transferMechanism;
            container = BuildOneValue(TWTY_UINT16, value);
        }
        break;

    default:
        return CapabilityResult::Failure(TWCC_CAPUNSUPPORTED);
    }

    if (container == nullptr)
    {
        return CapabilityResult::Failure(TWCC_LOWMEMORY);
    }

    capability->ConType = containerType;
    capability->hContainer = container;
    return CapabilityResult::Success();
}

CapabilityResult CapabilityStore::Set(pTW_CAPABILITY capability)
{
    TW_UINT32 packedItem = 0;

    switch (capability->Cap)
    {
    case CAP_XFERCOUNT:
    {
        if (!ReadCapabilityPackedItem(*capability, TWTY_INT16, packedItem))
        {
            return CapabilityResult::Failure(TWCC_CAPBADOPERATION);
        }

        const auto value = static_cast<TW_INT16>(static_cast<TW_UINT16>(packedItem));
        if (value == 0 || value < -1)
        {
            return CapabilityResult::Failure(TWCC_BADVALUE);
        }

        settings_.transferCount = value;
        return CapabilityResult::Success();
    }

    case CAP_DUPLEXENABLED:
        if (!ReadCapabilityPackedItem(*capability, TWTY_BOOL, packedItem))
        {
            return CapabilityResult::Failure(TWCC_CAPBADOPERATION);
        }
        if (packedItem != FALSE && packedItem != TRUE)
        {
            return CapabilityResult::Failure(TWCC_BADVALUE);
        }
        settings_.duplexEnabled = static_cast<TW_BOOL>(packedItem);
        return CapabilityResult::Success();

    case CAP_FEEDERENABLED:
    case CAP_UICONTROLLABLE:
    case CAP_DUPLEX:
        if (!ReadCapabilityPackedItem(*capability, TWTY_BOOL, packedItem))
        {
            return CapabilityResult::Failure(TWCC_CAPBADOPERATION);
        }
        if (packedItem != TRUE)
        {
            return CapabilityResult::Failure(TWCC_BADVALUE);
        }
        return CapabilityResult::Success();

    case ICAP_PIXELTYPE:
    {
        if (!ReadCapabilityPackedItem(*capability, TWTY_UINT16, packedItem))
        {
            return CapabilityResult::Failure(TWCC_CAPBADOPERATION);
        }

        const auto value = static_cast<TW_UINT16>(packedItem);
        if (!SetPixelTypeIfSupported(value))
        {
            return CapabilityResult::Failure(TWCC_BADVALUE);
        }

        return CapabilityResult::Success();
    }

    case ICAP_XRESOLUTION:
    case ICAP_YRESOLUTION:
    {
        if (!ReadCapabilityPackedItem(*capability, TWTY_FIX32, packedItem))
        {
            return CapabilityResult::Failure(TWCC_CAPBADOPERATION);
        }

        const TW_FIX32 value = UnpackFix32(packedItem);
        const bool accepted = capability->Cap == ICAP_XRESOLUTION
            ? SetXResolutionIfSupported(value)
            : SetYResolutionIfSupported(value);
        if (!accepted)
        {
            return CapabilityResult::Failure(TWCC_BADVALUE);
        }

        return CapabilityResult::Success();
    }

    case ICAP_SUPPORTEDSIZES:
    {
        if (!ReadCapabilityPackedItem(*capability, TWTY_UINT16, packedItem))
        {
            return CapabilityResult::Failure(TWCC_CAPBADOPERATION);
        }
        const auto value = static_cast<TW_UINT16>(packedItem);
        if (!SetPaperSizeIfSupported(value))
        {
            return CapabilityResult::Failure(TWCC_BADVALUE);
        }
        return CapabilityResult::Success();
    }

    case ICAP_AUTOMATICBORDERDETECTION:
    case ICAP_AUTOMATICDESKEW:
        if (!ReadCapabilityPackedItem(*capability, TWTY_BOOL, packedItem))
        {
            return CapabilityResult::Failure(TWCC_CAPBADOPERATION);
        }
        if (packedItem != TRUE)
        {
            return CapabilityResult::Failure(TWCC_BADVALUE);
        }
        return CapabilityResult::Success();

    case ICAP_XFERMECH:
    {
        if (!ReadCapabilityPackedItem(*capability, TWTY_UINT16, packedItem))
        {
            return CapabilityResult::Failure(TWCC_CAPBADOPERATION);
        }

        const auto value = static_cast<TW_UINT16>(packedItem);
        if (!Contains(kTransferMechanismValues, value))
        {
            return CapabilityResult::Failure(TWCC_BADVALUE);
        }

        settings_.transferMechanism = value;
        return CapabilityResult::Success();
    }

    case CAP_SUPPORTEDCAPS:
    case CAP_SUPPORTEDDATS:
        return CapabilityResult::Failure(TWCC_CAPBADOPERATION);

    default:
        return CapabilityResult::Failure(TWCC_CAPUNSUPPORTED);
    }
}

CapabilityResult CapabilityStore::Reset(pTW_CAPABILITY capability)
{
    const CapabilityResult resetResult = ResetValue(capability->Cap);
    if (resetResult.returnCode != TWRC_SUCCESS)
    {
        return resetResult;
    }

    return Get(MSG_GETCURRENT, capability);
}

CapabilityResult CapabilityStore::QuerySupport(pTW_CAPABILITY capability) const
{
    TW_UINT32 support = 0;

    switch (capability->Cap)
    {
    case CAP_SUPPORTEDCAPS:
    case CAP_SUPPORTEDDATS:
        support = kReadOnlyCapabilitySupport;
        break;
    case CAP_XFERCOUNT:
    case CAP_FEEDERENABLED:
    case CAP_UICONTROLLABLE:
    case CAP_DUPLEX:
    case CAP_DUPLEXENABLED:
    case ICAP_PIXELTYPE:
    case ICAP_XRESOLUTION:
    case ICAP_YRESOLUTION:
    case ICAP_SUPPORTEDSIZES:
    case ICAP_AUTOMATICBORDERDETECTION:
    case ICAP_AUTOMATICDESKEW:
    case ICAP_XFERMECH:
        support = kMutableCapabilitySupport;
        break;
    default:
        return CapabilityResult::Failure(TWCC_CAPUNSUPPORTED);
    }

    TW_HANDLE container = BuildOneValue(TWTY_INT32, support);
    if (container == nullptr)
    {
        return CapabilityResult::Failure(TWCC_LOWMEMORY);
    }

    capability->ConType = TWON_ONEVALUE;
    capability->hContainer = container;
    return CapabilityResult::Success();
}

CapabilityResult CapabilityStore::ResetAll() noexcept
{
    settings_.duplexEnabled = FALSE;
    settings_.pixelType = TWPT_RGB;
    settings_.paperSize = TWSS_A4LETTER;
    settings_.xResolution = MakeFix32(300);
    settings_.yResolution = MakeFix32(300);
    settings_.transferMechanism = TWSX_NATIVE;
    settings_.transferCount = -1;
    return CapabilityResult::Success();
}

CapabilityResult CapabilityStore::ResetValue(TW_UINT16 capability) noexcept
{
    switch (capability)
    {
    case CAP_XFERCOUNT:
        settings_.transferCount = -1;
        return CapabilityResult::Success();
    case CAP_FEEDERENABLED:
    case CAP_UICONTROLLABLE:
    case CAP_DUPLEX:
        return CapabilityResult::Success();
    case CAP_DUPLEXENABLED:
        settings_.duplexEnabled = FALSE;
        return CapabilityResult::Success();
    case ICAP_PIXELTYPE:
        settings_.pixelType = TWPT_RGB;
        return CapabilityResult::Success();
    case ICAP_SUPPORTEDSIZES:
        settings_.paperSize = TWSS_A4LETTER;
        return CapabilityResult::Success();
    case ICAP_XRESOLUTION:
        settings_.xResolution = MakeFix32(300);
        return CapabilityResult::Success();
    case ICAP_YRESOLUTION:
        settings_.yResolution = MakeFix32(300);
        return CapabilityResult::Success();
    case ICAP_AUTOMATICBORDERDETECTION:
    case ICAP_AUTOMATICDESKEW:
        return CapabilityResult::Success();
    case ICAP_XFERMECH:
        settings_.transferMechanism = TWSX_NATIVE;
        return CapabilityResult::Success();
    case CAP_SUPPORTEDCAPS:
    case CAP_SUPPORTEDDATS:
        return CapabilityResult::Failure(TWCC_CAPBADOPERATION);
    default:
        return CapabilityResult::Failure(TWCC_CAPUNSUPPORTED);
    }
}

} // namespace mbf::twain
