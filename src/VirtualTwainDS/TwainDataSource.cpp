#include "TwainDataSource.h"

#include "DiagnosticsLog.h"
#include "ImageDib.h"
#include "ScannerIpcClient.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string_view>
#include <utility>

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
constexpr std::uint32_t kUiStateFailureThreshold = 3;

constexpr TW_UINT32 kMutableCapabilitySupport =
    TWQC_GET | TWQC_SET | TWQC_GETDEFAULT | TWQC_GETCURRENT | TWQC_RESET;
constexpr TW_UINT32 kReadOnlyCapabilitySupport =
    TWQC_GET | TWQC_GETDEFAULT | TWQC_GETCURRENT;

void CopyTwainString(char* destination, size_t destinationSize, std::string_view source) noexcept
{
    if (destination == nullptr || destinationSize == 0)
    {
        return;
    }

    const size_t bytesToCopy = (std::min)(destinationSize - 1, source.size());
    std::memset(destination, 0, destinationSize);
    std::memcpy(destination, source.data(), bytesToCopy);
}

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
        enumeration->ItemType == expectedItemType &&
        itemSize > 0 &&
        enumeration->NumItems > 0 &&
        enumeration->CurrentIndex < enumeration->NumItems;

    if (ok)
    {
        packedItem = 0;
        const auto* item = enumeration->ItemList + (enumeration->CurrentIndex * itemSize);
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

bool LockTwainMemory(TW_MEMORY& memory, BYTE*& data, bool& unlockHandle) noexcept
{
    data = nullptr;
    unlockHandle = false;

    if (memory.Length == 0 || memory.TheMem == nullptr)
    {
        return false;
    }

    if ((memory.Flags & TWMF_POINTER) != 0)
    {
        data = static_cast<BYTE*>(memory.TheMem);
        return data != nullptr;
    }

    if ((memory.Flags & TWMF_HANDLE) != 0)
    {
        data = static_cast<BYTE*>(GlobalLock(static_cast<HGLOBAL>(memory.TheMem)));
        unlockHandle = data != nullptr;
        return data != nullptr;
    }

    return false;
}

void UnlockTwainMemory(TW_MEMORY& memory, bool unlockHandle) noexcept
{
    if (unlockHandle && memory.TheMem != nullptr)
    {
        GlobalUnlock(static_cast<HGLOBAL>(memory.TheMem));
    }
}

bool TryMapPixelType(const std::wstring& value, TW_UINT16& pixelType) noexcept
{
    if (value == L"BW")
    {
        pixelType = TWPT_BW;
        return true;
    }
    if (value == L"GRAY")
    {
        pixelType = TWPT_GRAY;
        return true;
    }
    if (value == L"RGB")
    {
        pixelType = TWPT_RGB;
        return true;
    }

    return false;
}

bool TryMapPaperSize(const std::wstring& value, TW_UINT16& paperSize) noexcept
{
    if (value == L"A4" || value == L"A4LETTER")
    {
        paperSize = TWSS_A4LETTER;
        return true;
    }
    if (value == L"A3")
    {
        paperSize = TWSS_A3;
        return true;
    }

    return false;
}

std::wstring PixelTypeToProtocol(TW_UINT16 pixelType)
{
    switch (pixelType)
    {
    case TWPT_BW:
        return L"BW";
    case TWPT_GRAY:
        return L"GRAY";
    case TWPT_RGB:
    default:
        return L"RGB";
    }
}

std::wstring PaperSizeToProtocol(TW_UINT16 paperSize)
{
    switch (paperSize)
    {
    case TWSS_A3:
        return L"A3";
    case TWSS_A4LETTER:
    default:
        return L"A4";
    }
}

std::uint32_t Fix32WholeOrDefault(TW_FIX32 value, std::uint32_t fallback) noexcept
{
    if (value.Frac == 0 && value.Whole > 0)
    {
        return static_cast<std::uint32_t>(value.Whole);
    }

    return fallback;
}

bool FileExists(const std::wstring& path) noexcept
{
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring DirectoryName(std::wstring path)
{
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
    {
        path.pop_back();
    }

    const size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos)
    {
        return {};
    }

    return path.substr(0, separator);
}

std::optional<std::wstring> ReadEnvironmentString(const wchar_t* name)
{
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required > 0)
    {
        std::wstring value(required, L'\0');
        const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
        if (written > 0 && written < required)
        {
            value.resize(written);
            return value;
        }
    }

    auto readRegistryString = [name](HKEY root, const wchar_t* subKey) -> std::optional<std::wstring>
    {
        DWORD bytes = 0;
        DWORD type = 0;
        const LSTATUS sizeStatus = RegGetValueW(
            root,
            subKey,
            name,
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            &type,
            nullptr,
            &bytes);
        if (sizeStatus != ERROR_SUCCESS || bytes < sizeof(wchar_t))
        {
            return std::nullopt;
        }

        std::wstring value(bytes / sizeof(wchar_t), L'\0');
        const LSTATUS readStatus = RegGetValueW(
            root,
            subKey,
            name,
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            &type,
            value.data(),
            &bytes);
        if (readStatus != ERROR_SUCCESS || bytes < sizeof(wchar_t))
        {
            return std::nullopt;
        }

        value.resize((bytes / sizeof(wchar_t)) - 1U);
        if (type != REG_EXPAND_SZ || value.empty())
        {
            return value;
        }

        const DWORD expandedRequired = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (expandedRequired == 0)
        {
            return value;
        }

        std::wstring expanded(expandedRequired, L'\0');
        const DWORD expandedWritten =
            ExpandEnvironmentStringsW(value.c_str(), expanded.data(), expandedRequired);
        if (expandedWritten == 0 || expandedWritten > expandedRequired)
        {
            return value;
        }

        expanded.resize(expandedWritten - 1U);
        return expanded;
    };

    if (auto value = readRegistryString(HKEY_CURRENT_USER, L"Environment"))
    {
        return value;
    }

    if (auto value = readRegistryString(
            HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"))
    {
        return value;
    }

    return std::nullopt;
}

bool EnvironmentFlagEnabled(const wchar_t* name)
{
    const auto value = ReadEnvironmentString(name);
    return value.has_value() &&
           (*value == L"1" || *value == L"true" || *value == L"TRUE" || *value == L"yes" || *value == L"YES");
}

std::optional<std::wstring> CurrentModuleDirectory()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&CurrentModuleDirectory),
            &module))
    {
        return std::nullopt;
    }

    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
        {
            return std::nullopt;
        }
        if (length < buffer.size() - 1)
        {
            return DirectoryName(std::wstring(buffer.data(), length));
        }

        buffer.resize(buffer.size() * 2U);
    }
}

std::vector<std::wstring> BuildUiExecutableCandidates()
{
    constexpr wchar_t kUiExeName[] = L"mbfTwain.VirtualScannerConfig.exe";
    std::vector<std::wstring> candidates;

    if (auto configuredPath = ReadEnvironmentString(L"MBF_TWAIN_UI_EXE"))
    {
        candidates.push_back(*configuredPath);
    }

    auto directory = CurrentModuleDirectory();
    if (!directory)
    {
        return candidates;
    }

    candidates.push_back(*directory + L"\\" + kUiExeName);

    std::wstring current = *directory;
    for (int depth = 0; depth < 8 && !current.empty(); ++depth)
    {
        candidates.push_back(
            current + L"\\src\\VirtualScannerConfig\\bin\\Release\\net10.0-windows\\" + kUiExeName);
        current = DirectoryName(current);
    }

    return candidates;
}

std::optional<std::wstring> FindUiExecutable()
{
    for (const std::wstring& candidate : BuildUiExecutableCandidates())
    {
        const bool exists = FileExists(candidate);
        mbf::twain::diagnostics::AppendLine(
            L"DS",
            L"UI candidate " + candidate + (exists ? L" exists" : L" missing"));
        if (exists)
        {
            return candidate;
        }
    }

    mbf::twain::diagnostics::AppendLine(L"DS", L"No UI executable candidate was found");
    return std::nullopt;
}

std::wstring TwainStateName(mbf::twain::TwainState state)
{
    switch (state)
    {
    case mbf::twain::TwainState::SourceLoaded:
        return L"SourceLoaded";
    case mbf::twain::TwainState::SourceOpened:
        return L"SourceOpened";
    case mbf::twain::TwainState::SourceEnabled:
        return L"SourceEnabled";
    case mbf::twain::TwainState::TransferReady:
        return L"TransferReady";
    case mbf::twain::TwainState::Transferring:
        return L"Transferring";
    default:
        return L"Unknown";
    }
}

std::wstring DataGroupName(TW_UINT32 dataGroup)
{
    switch (dataGroup)
    {
    case DG_CONTROL:
        return L"DG_CONTROL";
    case DG_IMAGE:
        return L"DG_IMAGE";
    default:
        return L"DG_" + std::to_wstring(dataGroup);
    }
}

std::wstring DataArgumentTypeName(TW_UINT16 dataArgumentType)
{
    switch (dataArgumentType)
    {
    case DAT_IDENTITY:
        return L"DAT_IDENTITY";
    case DAT_CAPABILITY:
        return L"DAT_CAPABILITY";
    case DAT_USERINTERFACE:
        return L"DAT_USERINTERFACE";
    case DAT_XFERGROUP:
        return L"DAT_XFERGROUP";
    case DAT_EVENT:
        return L"DAT_EVENT";
    case DAT_PENDINGXFERS:
        return L"DAT_PENDINGXFERS";
    case DAT_SETUPMEMXFER:
        return L"DAT_SETUPMEMXFER";
    case DAT_STATUS:
        return L"DAT_STATUS";
    case DAT_ENTRYPOINT:
        return L"DAT_ENTRYPOINT";
    case DAT_IMAGEINFO:
        return L"DAT_IMAGEINFO";
    case DAT_IMAGENATIVEXFER:
        return L"DAT_IMAGENATIVEXFER";
    case DAT_IMAGEMEMXFER:
        return L"DAT_IMAGEMEMXFER";
    default:
        return L"DAT_" + std::to_wstring(dataArgumentType);
    }
}

std::wstring MessageName(TW_UINT16 message)
{
    switch (message)
    {
    case MSG_GET:
        return L"MSG_GET";
    case MSG_GETCURRENT:
        return L"MSG_GETCURRENT";
    case MSG_GETDEFAULT:
        return L"MSG_GETDEFAULT";
    case MSG_GETFIRST:
        return L"MSG_GETFIRST";
    case MSG_GETNEXT:
        return L"MSG_GETNEXT";
    case MSG_SET:
        return L"MSG_SET";
    case MSG_RESET:
        return L"MSG_RESET";
    case MSG_QUERYSUPPORT:
        return L"MSG_QUERYSUPPORT";
    case MSG_OPENDS:
        return L"MSG_OPENDS";
    case MSG_CLOSEDS:
        return L"MSG_CLOSEDS";
    case MSG_ENABLEDS:
        return L"MSG_ENABLEDS";
    case MSG_ENABLEDSUIONLY:
        return L"MSG_ENABLEDSUIONLY";
    case MSG_DISABLEDS:
        return L"MSG_DISABLEDS";
    case MSG_PROCESSEVENT:
        return L"MSG_PROCESSEVENT";
    case MSG_ENDXFER:
        return L"MSG_ENDXFER";
    case MSG_RESETALL:
        return L"MSG_RESETALL";
    default:
        return L"MSG_" + std::to_wstring(message);
    }
}

std::wstring DescribeTriplet(
    TW_UINT32 dataGroup,
    TW_UINT16 dataArgumentType,
    TW_UINT16 message,
    TW_MEMREF data)
{
    std::wostringstream text;
    text << DataGroupName(dataGroup)
         << L"/"
         << DataArgumentTypeName(dataArgumentType)
         << L"/"
         << MessageName(message)
         << L" data=0x"
         << std::hex
         << reinterpret_cast<std::uintptr_t>(data);

    if (dataArgumentType == DAT_USERINTERFACE && data != nullptr)
    {
        const auto* userInterface = static_cast<const TW_USERINTERFACE*>(data);
        text << std::dec
             << L" showUI=" << static_cast<int>(userInterface->ShowUI)
             << L" modalUI=" << static_cast<int>(userInterface->ModalUI)
             << L" hParent=0x"
             << std::hex
             << reinterpret_cast<std::uintptr_t>(userInterface->hParent);
    }

    return text.str();
}

} // namespace

namespace mbf::twain
{

VirtualTwainDataSource& VirtualTwainDataSource::Instance()
{
    static VirtualTwainDataSource instance;
    return instance;
}

VirtualTwainDataSource::VirtualTwainDataSource()
    : identity_(BuildSourceIdentity())
{
    lastStatus_.ConditionCode = TWCC_SUCCESS;
    lastStatus_.Data = 0;
    settings_.xResolution = MakeFix32(300);
    settings_.yResolution = MakeFix32(300);
    diagnostics::AppendLine(
        L"DS",
        L"VirtualTwainDataSource initialized logPath=" + diagnostics::LogFilePath());
}

VirtualTwainDataSource::~VirtualTwainDataSource()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transferReadyWatcherStop_ = true;
        transferReadyWatcherActive_ = false;
        ++transferReadyWatcherGeneration_;
    }

    transferReadyWatcherCv_.notify_all();
    if (transferReadyWatcher_.joinable())
    {
        transferReadyWatcher_.join();
    }
}

TW_UINT16 VirtualTwainDataSource::Entry(
    pTW_IDENTITY origin,
    TW_UINT32 dataGroup,
    TW_UINT16 dataArgumentType,
    TW_UINT16 message,
    TW_MEMREF data)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::wstring triplet = DescribeTriplet(dataGroup, dataArgumentType, message, data);
    diagnostics::AppendLine(
        L"DS",
        L"Entry begin state=" + TwainStateName(state_) + L" " + triplet);

    TW_UINT16 result = TWRC_FAILURE;

    if (dataGroup == DG_IMAGE)
    {
        result = HandleImage(dataArgumentType, message, data);
    }
    else if (dataGroup != DG_CONTROL)
    {
        result = Fail(TWCC_BADPROTOCOL);
    }
    else
    {
        switch (dataArgumentType)
        {
        case DAT_IDENTITY:
            result = HandleIdentity(message, origin, data);
            break;
        case DAT_CAPABILITY:
            result = HandleCapability(message, data);
            break;
        case DAT_USERINTERFACE:
            result = HandleUserInterface(message, data);
            break;
        case DAT_EVENT:
            result = HandleEvent(message, data);
            break;
        case DAT_PENDINGXFERS:
            result = HandlePendingTransfers(message, data);
            break;
        case DAT_SETUPMEMXFER:
            result = HandleSetupMemoryTransfer(message, data);
            break;
        case DAT_XFERGROUP:
            result = HandleTransferGroup(message, data);
            break;
        case DAT_STATUS:
            result = HandleStatus(message, data);
            break;
        case DAT_ENTRYPOINT:
            result = HandleEntryPoint(message, data);
            break;
        default:
            result = Fail(TWCC_BADPROTOCOL);
            break;
        }
    }

    diagnostics::AppendLine(
        L"DS",
        L"Entry end state=" + TwainStateName(state_) +
            L" rc=" + std::to_wstring(result) +
            L" condition=" + std::to_wstring(lastStatus_.ConditionCode) +
            L" " + triplet);
    return result;
}

TW_UINT16 VirtualTwainDataSource::HandleIdentity(
    TW_UINT16 message,
    pTW_IDENTITY origin,
    TW_MEMREF data)
{
    auto* requestedIdentity = static_cast<pTW_IDENTITY>(data);

    switch (message)
    {
    case MSG_GET:
        if (requestedIdentity == nullptr)
        {
            return Fail(TWCC_BADVALUE);
        }

        *requestedIdentity = identity_;
        return Succeed();

    case MSG_OPENDS:
        if (requestedIdentity == nullptr)
        {
            return Fail(TWCC_BADVALUE);
        }
        if (state_ != TwainState::SourceLoaded)
        {
            return Fail(TWCC_SEQERROR);
        }

        if (origin != nullptr)
        {
            openOrigin_ = *origin;
        }
        else
        {
            openOrigin_.reset();
        }

        identity_.Id = requestedIdentity->Id;
        *requestedIdentity = identity_;
        state_ = TwainState::SourceOpened;
        return Succeed();

    case MSG_CLOSEDS:
        if (!IsAtLeast(TwainState::SourceOpened))
        {
            return Fail(TWCC_SEQERROR);
        }

        if (pendingIpcRevision_ != 0)
        {
            AcknowledgeScanIfComplete();
        }
        else
        {
            HideScanUiSession();
        }

        state_ = TwainState::SourceLoaded;
        StopTransferReadyWatcher();
        openOrigin_.reset();
        transferReady_ = false;
        transferReadyNotified_ = false;
        ClearTransferProgress();
        return Succeed();

    default:
        return Fail(TWCC_BADPROTOCOL);
    }
}

TW_UINT16 VirtualTwainDataSource::HandleCapability(TW_UINT16 message, TW_MEMREF data)
{
    if (!IsAtLeast(TwainState::SourceOpened))
    {
        return Fail(TWCC_CAPSEQERROR);
    }

    if (message == MSG_RESETALL)
    {
        return ResetAllCapabilities();
    }

    auto* capability = static_cast<pTW_CAPABILITY>(data);
    if (capability == nullptr)
    {
        return Fail(TWCC_BADVALUE);
    }

    switch (message)
    {
    case MSG_GET:
    case MSG_GETCURRENT:
    case MSG_GETDEFAULT:
        return GetCapability(message, capability);
    case MSG_SET:
        return SetCapability(capability);
    case MSG_RESET:
        return ResetCapability(capability);
    case MSG_QUERYSUPPORT:
        return QueryCapabilitySupport(capability);
    default:
        return Fail(TWCC_CAPBADOPERATION);
    }
}

TW_UINT16 VirtualTwainDataSource::HandleUserInterface(TW_UINT16 message, TW_MEMREF data)
{
    auto* userInterface = static_cast<pTW_USERINTERFACE>(data);
    if (userInterface == nullptr)
    {
        return Fail(TWCC_BADVALUE);
    }

    switch (message)
    {
    case MSG_ENABLEDS:
    case MSG_ENABLEDSUIONLY:
    {
        if (state_ != TwainState::SourceOpened)
        {
            return Fail(TWCC_SEQERROR);
        }

        const bool shouldShowUi =
            message == MSG_ENABLEDSUIONLY ||
            userInterface->ShowUI != FALSE ||
            EnvironmentFlagEnabled(L"MBF_TWAIN_FORCE_UI");

        diagnostics::AppendLine(
            L"DS",
            L"HandleUserInterface enable shouldShowUi=" + std::to_wstring(shouldShowUi ? 1U : 0U) +
                L" showUI=" + std::to_wstring(userInterface->ShowUI != FALSE ? 1U : 0U) +
                L" forceUi=" +
                std::to_wstring(EnvironmentFlagEnabled(L"MBF_TWAIN_FORCE_UI") ? 1U : 0U));

        transferReady_ = false;
        transferReadyNotified_ = false;
        awaitingUiSelection_ = false;
        closeDsRequest_ = false;
        closeDsRequestNotified_ = false;
        scanUiHiddenForCurrentTransfer_ = false;
        ClearTransferProgress();
        state_ = TwainState::SourceEnabled;

        if (shouldShowUi && !BeginUiScanSession(true))
        {
            state_ = TwainState::SourceOpened;
            diagnostics::AppendLine(
                L"DS",
                L"BeginUiScanSession failed during enable; reverting to SourceOpened");
            return Fail(TWCC_OPERATIONERROR);
        }

        if (!shouldShowUi && RefreshTransferReadyFromIpc())
        {
            transferReady_ = true;
            state_ = TwainState::TransferReady;
            NotifyTransferReady();
        }
        else if (shouldShowUi && message == MSG_ENABLEDS)
        {
            awaitingUiSelection_ = true;
            StartTransferReadyWatcher();
        }

        return Succeed();
    }

    case MSG_DISABLEDS:
        if (state_ == TwainState::SourceOpened)
        {
            return Succeed();
        }
        if (!IsAtLeast(TwainState::SourceEnabled))
        {
            return Fail(TWCC_SEQERROR);
        }

        transferReady_ = false;
        transferReadyNotified_ = false;
        StopTransferReadyWatcher();
        if (pendingIpcRevision_ != 0)
        {
            AcknowledgeScanIfComplete();
        }
        else
        {
            HideScanUiSession();
        }
        ClearTransferProgress();
        state_ = TwainState::SourceOpened;
        return Succeed();

    default:
        return Fail(TWCC_BADPROTOCOL);
    }
}

TW_UINT16 VirtualTwainDataSource::HandleEvent(TW_UINT16 message, TW_MEMREF data)
{
    if (message != MSG_PROCESSEVENT)
    {
        return Fail(TWCC_BADPROTOCOL);
    }
    if (!IsAtLeast(TwainState::SourceEnabled))
    {
        return Fail(TWCC_SEQERROR);
    }

    auto* event = static_cast<pTW_EVENT>(data);
    if (event == nullptr)
    {
        return Fail(TWCC_BADVALUE);
    }

    event->TWMessage = MSG_NULL;

    if (!transferReady_ && !closeDsRequest_)
    {
        transferReady_ = RefreshTransferReadyFromIpc();
    }

    if (closeDsRequest_)
    {
        RollbackCanceledUiEnable();
        event->TWMessage = MSG_CLOSEDSREQ;
        lastStatus_.ConditionCode = TWCC_SUCCESS;
        lastStatus_.Data = 0;
        return TWRC_DSEVENT;
    }

    if (transferReady_)
    {
        state_ = TwainState::TransferReady;
        event->TWMessage = MSG_XFERREADY;
        lastStatus_.ConditionCode = TWCC_SUCCESS;
        lastStatus_.Data = 0;
        return TWRC_DSEVENT;
    }

    lastStatus_.ConditionCode = TWCC_SUCCESS;
    lastStatus_.Data = 0;
    return TWRC_NOTDSEVENT;
}

TW_UINT16 VirtualTwainDataSource::HandlePendingTransfers(TW_UINT16 message, TW_MEMREF data)
{
    auto* pendingTransfers = static_cast<pTW_PENDINGXFERS>(data);
    if (pendingTransfers == nullptr)
    {
        return Fail(TWCC_BADVALUE);
    }

    switch (message)
    {
    case MSG_GET:
    {
        if (state_ != TwainState::TransferReady && state_ != TwainState::Transferring)
        {
            return Fail(TWCC_SEQERROR);
        }

        const TW_UINT32 remaining =
            pendingTransferIndex_ < pendingImages_.size()
                ? static_cast<TW_UINT32>(pendingImages_.size() - pendingTransferIndex_)
                : 0;
        pendingTransfers->Count = static_cast<TW_UINT16>((std::min)(remaining, static_cast<TW_UINT32>(0xffff)));
        pendingTransfers->EOJ = 0;
        return Succeed();
    }

    case MSG_ENDXFER:
    {
        if (state_ != TwainState::Transferring && state_ != TwainState::TransferReady)
        {
            return Fail(TWCC_SEQERROR);
        }

        const TW_UINT32 remaining =
            pendingTransferIndex_ < pendingImages_.size()
                ? static_cast<TW_UINT32>(pendingImages_.size() - pendingTransferIndex_)
                : 0;
        pendingTransfers->Count = static_cast<TW_UINT16>((std::min)(remaining, static_cast<TW_UINT32>(0xffff)));
        pendingTransfers->EOJ = 0;

        if (remaining > 0)
        {
            transferReady_ = true;
            transferReadyNotified_ = false;
            hasCurrentTransferImage_ = false;
            state_ = TwainState::TransferReady;
        }
        else
        {
            transferReady_ = false;
            transferReadyNotified_ = false;
            hasCurrentTransferImage_ = false;
            ResetMemoryTransfer();
            state_ = TwainState::SourceEnabled;
            AcknowledgeScanIfComplete();
        }

        return Succeed();
    }

    case MSG_RESET:
    {
        const TwainState resetState =
            state_ == TwainState::SourceOpened ? TwainState::SourceOpened : TwainState::SourceEnabled;
        pendingTransfers->Count = 0;
        pendingTransfers->EOJ = 0;
        transferReady_ = false;
        transferReadyNotified_ = false;
        awaitingUiSelection_ = false;
        closeDsRequest_ = false;
        closeDsRequestNotified_ = false;
        pendingTransferIndex_ = 0;
        pendingImages_.clear();
        hasCurrentTransferImage_ = false;
        currentTransferImageIndex_ = 0;
        ResetMemoryTransfer();
        state_ = resetState;
        AcknowledgeScanIfComplete();
        return Succeed();
    }

    default:
        return Fail(TWCC_BADPROTOCOL);
    }
}

TW_UINT16 VirtualTwainDataSource::HandleTransferGroup(TW_UINT16 message, TW_MEMREF data)
{
    if (!IsAtLeast(TwainState::SourceOpened))
    {
        return Fail(TWCC_SEQERROR);
    }

    auto* transferGroup = static_cast<pTW_UINT32>(data);
    if (transferGroup == nullptr)
    {
        return Fail(TWCC_BADVALUE);
    }

    switch (message)
    {
    case MSG_GET:
        *transferGroup = DG_IMAGE;
        return Succeed();
    case MSG_SET:
        return *transferGroup == DG_IMAGE ? Succeed() : Fail(TWCC_BADVALUE);
    default:
        return Fail(TWCC_BADPROTOCOL);
    }
}

TW_UINT16 VirtualTwainDataSource::HandleSetupMemoryTransfer(TW_UINT16 message, TW_MEMREF data)
{
    if (message != MSG_GET)
    {
        return Fail(TWCC_BADPROTOCOL);
    }
    if (!IsAtLeast(TwainState::SourceOpened))
    {
        return Fail(TWCC_SEQERROR);
    }

    auto* setup = static_cast<pTW_SETUPMEMXFER>(data);
    if (setup == nullptr)
    {
        return Fail(TWCC_BADVALUE);
    }

    setup->MinBufSize = 32U * 1024U;
    setup->Preferred = 256U * 1024U;
    setup->MaxBufSize = 1024U * 1024U;
    return Succeed();
}

TW_UINT16 VirtualTwainDataSource::HandleImage(
    TW_UINT16 dataArgumentType,
    TW_UINT16 message,
    TW_MEMREF data)
{
    switch (dataArgumentType)
    {
    case DAT_IMAGEINFO:
        return HandleImageInfo(message, data);
    case DAT_IMAGENATIVEXFER:
        return HandleImageNativeTransfer(message, data);
    case DAT_IMAGEMEMXFER:
        return HandleImageMemoryTransfer(message, data);
    default:
        return Fail(TWCC_BADPROTOCOL);
    }
}

TW_UINT16 VirtualTwainDataSource::HandleImageInfo(TW_UINT16 message, TW_MEMREF data)
{
    if (message != MSG_GET)
    {
        return Fail(TWCC_BADPROTOCOL);
    }
    if (!IsAtLeast(TwainState::TransferReady))
    {
        return Fail(TWCC_SEQERROR);
    }

    auto* imageInfo = static_cast<pTW_IMAGEINFO>(data);
    if (imageInfo == nullptr)
    {
        return Fail(TWCC_BADVALUE);
    }

    TW_UINT32 imageIndex = 0;
    if (!TryResolveImageInfoIndex(imageIndex) ||
        !FillImageInfo(imageIndex, imageInfo))
    {
        return Fail(TWCC_OPERATIONERROR);
    }

    return Succeed();
}

TW_UINT16 VirtualTwainDataSource::HandleImageNativeTransfer(TW_UINT16 message, TW_MEMREF data)
{
    if (message != MSG_GET)
    {
        return Fail(TWCC_BADPROTOCOL);
    }
    if (state_ != TwainState::TransferReady)
    {
        return Fail(TWCC_SEQERROR);
    }
    if (pendingTransferIndex_ >= pendingImages_.size())
    {
        return Fail(TWCC_SEQERROR);
    }

    auto* outputHandle = static_cast<TW_HANDLE*>(data);
    if (outputHandle == nullptr)
    {
        return Fail(TWCC_BADVALUE);
    }

    DecodedImageInfo decodedInfo{};
    const TW_UINT32 imageIndex = pendingTransferIndex_;
    TW_HANDLE dib = ImageDib::BuildNativeDib(
        pendingImages_[imageIndex],
        settings_.pixelType,
        decodedInfo);
    if (dib == nullptr)
    {
        return Fail(TWCC_OPERATIONERROR);
    }

    *outputHandle = dib;
    ++pendingTransferIndex_;
    currentTransferImageIndex_ = imageIndex;
    hasCurrentTransferImage_ = true;
    state_ = TwainState::Transferring;
    transferReady_ = pendingTransferIndex_ < pendingImages_.size();
    HideScanUiIfTransferStarted();
    lastStatus_.ConditionCode = TWCC_SUCCESS;
    lastStatus_.Data = 0;
    return TWRC_XFERDONE;
}

TW_UINT16 VirtualTwainDataSource::HandleImageMemoryTransfer(TW_UINT16 message, TW_MEMREF data)
{
    if (message != MSG_GET)
    {
        return Fail(TWCC_BADPROTOCOL);
    }
    if (state_ != TwainState::TransferReady && state_ != TwainState::Transferring)
    {
        return Fail(TWCC_SEQERROR);
    }

    auto* transfer = static_cast<pTW_IMAGEMEMXFER>(data);
    if (transfer == nullptr)
    {
        return Fail(TWCC_BADVALUE);
    }

    if (!EnsureMemoryTransferReady())
    {
        return Fail(TWCC_OPERATIONERROR);
    }

    BYTE* destination = nullptr;
    bool unlockHandle = false;
    if (!LockTwainMemory(transfer->Memory, destination, unlockHandle))
    {
        return Fail(TWCC_BADVALUE);
    }

    const TW_UINT32 bytesPerRow = memoryTransfer_.image.bytesPerRow;
    if (bytesPerRow == 0 || transfer->Memory.Length < bytesPerRow)
    {
        UnlockTwainMemory(transfer->Memory, unlockHandle);
        return Fail(TWCC_BADVALUE);
    }

    const TW_UINT32 rowsRemaining = memoryTransfer_.image.height - memoryTransfer_.nextRow;
    const TW_UINT32 rowsThatFit = transfer->Memory.Length / bytesPerRow;
    const TW_UINT32 rowsToWrite = (std::min)(rowsRemaining, rowsThatFit);
    const TW_UINT32 bytesToWrite = rowsToWrite * bytesPerRow;
    if (rowsToWrite == 0 || bytesToWrite == 0)
    {
        UnlockTwainMemory(transfer->Memory, unlockHandle);
        return Fail(TWCC_BADVALUE);
    }

    const BYTE* source = memoryTransfer_.image.pixels.data() +
        (static_cast<size_t>(memoryTransfer_.nextRow) * bytesPerRow);
    std::memcpy(destination, source, bytesToWrite);
    UnlockTwainMemory(transfer->Memory, unlockHandle);

    transfer->Compression = TWCP_NONE;
    transfer->BytesPerRow = bytesPerRow;
    transfer->Columns = memoryTransfer_.image.width;
    transfer->Rows = rowsToWrite;
    transfer->XOffset = 0;
    transfer->YOffset = memoryTransfer_.nextRow;
    transfer->BytesWritten = bytesToWrite;

    memoryTransfer_.nextRow += rowsToWrite;
    currentTransferImageIndex_ = memoryTransfer_.imageIndex;
    hasCurrentTransferImage_ = true;
    state_ = TwainState::Transferring;
    lastStatus_.ConditionCode = TWCC_SUCCESS;
    lastStatus_.Data = 0;

    if (memoryTransfer_.nextRow >= memoryTransfer_.image.height)
    {
        ++pendingTransferIndex_;
        transferReady_ = pendingTransferIndex_ < pendingImages_.size();
        ResetMemoryTransfer();
        HideScanUiIfTransferStarted();
        return TWRC_XFERDONE;
    }

    return TWRC_SUCCESS;
}

TW_UINT16 VirtualTwainDataSource::HandleStatus(TW_UINT16 message, TW_MEMREF data)
{
    if (message != MSG_GET)
    {
        return Fail(TWCC_BADPROTOCOL);
    }

    auto* status = static_cast<pTW_STATUS>(data);
    if (status == nullptr)
    {
        return Fail(TWCC_BADVALUE);
    }

    *status = lastStatus_;
    return TWRC_SUCCESS;
}

TW_UINT16 VirtualTwainDataSource::HandleEntryPoint(TW_UINT16 message, TW_MEMREF data)
{
    if (message != MSG_SET)
    {
        return Fail(TWCC_BADPROTOCOL);
    }

    auto* entryPoint = static_cast<pTW_ENTRYPOINT>(data);
    if (entryPoint == nullptr)
    {
        return Fail(TWCC_BADVALUE);
    }

    entryPoint_ = *entryPoint;
    return Succeed();
}

TW_UINT16 VirtualTwainDataSource::GetCapability(TW_UINT16 message, pTW_CAPABILITY capability)
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
        return Fail(TWCC_CAPUNSUPPORTED);
    }

    if (container == nullptr)
    {
        return Fail(TWCC_LOWMEMORY);
    }

    capability->ConType = containerType;
    capability->hContainer = container;
    return Succeed();
}

TW_UINT16 VirtualTwainDataSource::SetCapability(pTW_CAPABILITY capability)
{
    TW_UINT32 packedItem = 0;

    switch (capability->Cap)
    {
    case CAP_XFERCOUNT:
    {
        if (!ReadCapabilityPackedItem(*capability, TWTY_INT16, packedItem))
        {
            return Fail(TWCC_CAPBADOPERATION);
        }

        const auto value = static_cast<TW_INT16>(static_cast<TW_UINT16>(packedItem));
        if (value == 0 || value < -1)
        {
            return Fail(TWCC_BADVALUE);
        }

        settings_.transferCount = value;
        return Succeed();
    }

    case CAP_DUPLEXENABLED:
        if (!ReadCapabilityPackedItem(*capability, TWTY_BOOL, packedItem))
        {
            return Fail(TWCC_CAPBADOPERATION);
        }
        if (packedItem != FALSE && packedItem != TRUE)
        {
            return Fail(TWCC_BADVALUE);
        }
        settings_.duplexEnabled = static_cast<TW_BOOL>(packedItem);
        return Succeed();

    case CAP_FEEDERENABLED:
    case CAP_UICONTROLLABLE:
    case CAP_DUPLEX:
        if (!ReadCapabilityPackedItem(*capability, TWTY_BOOL, packedItem))
        {
            return Fail(TWCC_CAPBADOPERATION);
        }
        if (packedItem != TRUE)
        {
            return Fail(TWCC_BADVALUE);
        }
        return Succeed();

    case ICAP_PIXELTYPE:
    {
        if (!ReadCapabilityPackedItem(*capability, TWTY_UINT16, packedItem))
        {
            return Fail(TWCC_CAPBADOPERATION);
        }

        const auto value = static_cast<TW_UINT16>(packedItem);
        if (std::find(kPixelTypeValues.begin(), kPixelTypeValues.end(), value) == kPixelTypeValues.end())
        {
            return Fail(TWCC_BADVALUE);
        }

        settings_.pixelType = value;
        return Succeed();
    }

    case ICAP_XRESOLUTION:
    case ICAP_YRESOLUTION:
    {
        if (!ReadCapabilityPackedItem(*capability, TWTY_FIX32, packedItem))
        {
            return Fail(TWCC_CAPBADOPERATION);
        }

        const TW_FIX32 value = UnpackFix32(packedItem);
        if (!ContainsFix32WholeValue(value))
        {
            return Fail(TWCC_BADVALUE);
        }

        if (capability->Cap == ICAP_XRESOLUTION)
        {
            settings_.xResolution = value;
        }
        else
        {
            settings_.yResolution = value;
        }
        return Succeed();
    }

    case ICAP_SUPPORTEDSIZES:
    {
        if (!ReadCapabilityPackedItem(*capability, TWTY_UINT16, packedItem))
        {
            return Fail(TWCC_CAPBADOPERATION);
        }
        const auto value = static_cast<TW_UINT16>(packedItem);
        if (std::find(kSupportedSizesValues.begin(), kSupportedSizesValues.end(), value) == kSupportedSizesValues.end())
        {
            return Fail(TWCC_BADVALUE);
        }
        settings_.paperSize = value;
        return Succeed();
    }

    case ICAP_AUTOMATICBORDERDETECTION:
    case ICAP_AUTOMATICDESKEW:
        if (!ReadCapabilityPackedItem(*capability, TWTY_BOOL, packedItem))
        {
            return Fail(TWCC_CAPBADOPERATION);
        }
        if (packedItem != TRUE)
        {
            return Fail(TWCC_BADVALUE);
        }
        return Succeed();

    case ICAP_XFERMECH:
    {
        if (!ReadCapabilityPackedItem(*capability, TWTY_UINT16, packedItem))
        {
            return Fail(TWCC_CAPBADOPERATION);
        }

        const auto value = static_cast<TW_UINT16>(packedItem);
        if (std::find(
                kTransferMechanismValues.begin(),
                kTransferMechanismValues.end(),
                value) == kTransferMechanismValues.end())
        {
            return Fail(TWCC_BADVALUE);
        }

        settings_.transferMechanism = value;
        return Succeed();
    }

    case CAP_SUPPORTEDCAPS:
    case CAP_SUPPORTEDDATS:
        return Fail(TWCC_CAPBADOPERATION);

    default:
        return Fail(TWCC_CAPUNSUPPORTED);
    }
}

TW_UINT16 VirtualTwainDataSource::ResetCapability(pTW_CAPABILITY capability)
{
    const TW_UINT16 resetResult = ResetCapabilityValue(capability->Cap);
    if (resetResult != TWRC_SUCCESS)
    {
        return resetResult;
    }

    return GetCapability(MSG_GETCURRENT, capability);
}

TW_UINT16 VirtualTwainDataSource::QueryCapabilitySupport(pTW_CAPABILITY capability)
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
        return Fail(TWCC_CAPUNSUPPORTED);
    }

    TW_HANDLE container = BuildOneValue(TWTY_INT32, support);
    if (container == nullptr)
    {
        return Fail(TWCC_LOWMEMORY);
    }

    capability->ConType = TWON_ONEVALUE;
    capability->hContainer = container;
    return Succeed();
}

TW_UINT16 VirtualTwainDataSource::ResetAllCapabilities() noexcept
{
    settings_.duplexEnabled = FALSE;
    settings_.pixelType = TWPT_RGB;
    settings_.paperSize = TWSS_A4LETTER;
    settings_.xResolution = MakeFix32(300);
    settings_.yResolution = MakeFix32(300);
    settings_.transferMechanism = TWSX_NATIVE;
    settings_.transferCount = -1;
    return Succeed();
}

TW_UINT16 VirtualTwainDataSource::ResetCapabilityValue(TW_UINT16 capability) noexcept
{
    switch (capability)
    {
    case CAP_XFERCOUNT:
        settings_.transferCount = -1;
        return Succeed();
    case CAP_FEEDERENABLED:
    case CAP_UICONTROLLABLE:
    case CAP_DUPLEX:
        return Succeed();
    case CAP_DUPLEXENABLED:
        settings_.duplexEnabled = FALSE;
        return Succeed();
    case ICAP_PIXELTYPE:
        settings_.pixelType = TWPT_RGB;
        return Succeed();
    case ICAP_SUPPORTEDSIZES:
        settings_.paperSize = TWSS_A4LETTER;
        return Succeed();
    case ICAP_XRESOLUTION:
        settings_.xResolution = MakeFix32(300);
        return Succeed();
    case ICAP_YRESOLUTION:
        settings_.yResolution = MakeFix32(300);
        return Succeed();
    case ICAP_AUTOMATICBORDERDETECTION:
    case ICAP_AUTOMATICDESKEW:
        return Succeed();
    case ICAP_XFERMECH:
        settings_.transferMechanism = TWSX_NATIVE;
        return Succeed();
    case CAP_SUPPORTEDCAPS:
    case CAP_SUPPORTEDDATS:
        return Fail(TWCC_CAPBADOPERATION);
    default:
        return Fail(TWCC_CAPUNSUPPORTED);
    }
}

bool VirtualTwainDataSource::RefreshTransferReadyFromIpc()
{
    ScannerIpcClient client;
    ScannerIpcState ipcState{};
    if (!client.TryGetState(ipcState, 30))
    {
        diagnostics::AppendLine(L"DS", L"RefreshTransferReadyFromIpc GET_STATE failed");
        return false;
    }

    diagnostics::AppendLine(
        L"DS",
        L"RefreshTransferReadyFromIpc revision=" + std::to_wstring(ipcState.revision) +
            L" scan=" + std::to_wstring(ipcState.scanRequested ? 1U : 0U) +
            L" images=" + std::to_wstring(ipcState.selectedImages.size()) +
            L" pixel=" + ipcState.pixelType +
            L" paper=" + ipcState.paperSize +
            L" xres=" + std::to_wstring(ipcState.xResolution) +
            L" yres=" + std::to_wstring(ipcState.yResolution));

    ApplyScannerSettingsFromIpc(ipcState);

    if (!ipcState.scanRequested || ipcState.selectedImages.empty())
    {
        diagnostics::AppendLine(L"DS", L"Transfer not ready yet: scanRequested=0 or no images selected");
        return false;
    }

    CommitTransferReadyFromIpc(std::move(ipcState));
    return true;
}

void VirtualTwainDataSource::ApplyScannerSettingsFromIpc(const ScannerIpcState& ipcState)
{
    TW_UINT16 mappedPixelType = settings_.pixelType;
    if (TryMapPixelType(ipcState.pixelType, mappedPixelType))
    {
        settings_.pixelType = mappedPixelType;
    }

    TW_UINT16 mappedPaperSize = settings_.paperSize;
    if (TryMapPaperSize(ipcState.paperSize, mappedPaperSize))
    {
        settings_.paperSize = mappedPaperSize;
    }

    const auto xResolution = MakeFix32(static_cast<TW_INT16>(ipcState.xResolution));
    if (ContainsFix32WholeValue(xResolution))
    {
        settings_.xResolution = xResolution;
    }

    const auto yResolution = MakeFix32(static_cast<TW_INT16>(ipcState.yResolution));
    if (ContainsFix32WholeValue(yResolution))
    {
        settings_.yResolution = yResolution;
    }

    settings_.duplexEnabled = ipcState.duplexEnabled ? TRUE : FALSE;
}

void VirtualTwainDataSource::CommitTransferReadyFromIpc(ScannerIpcState&& ipcState)
{
    pendingIpcRevision_ = ipcState.revision;
    pendingTransferIndex_ = 0;
    hasCurrentTransferImage_ = false;
    currentTransferImageIndex_ = 0;
    awaitingUiSelection_ = false;
    closeDsRequest_ = false;
    closeDsRequestNotified_ = false;
    scanUiHiddenForCurrentTransfer_ = false;
    transferReadyNotified_ = false;
    ResetMemoryTransfer();
    pendingImages_ = std::move(ipcState.selectedImages);
    if (settings_.transferCount > 0)
    {
        const size_t requestedImageCount = static_cast<size_t>(settings_.transferCount);
        if (pendingImages_.size() > requestedImageCount)
        {
            diagnostics::AppendLine(
                L"DS",
                L"CAP_XFERCOUNT truncating scan revision=" + std::to_wstring(pendingIpcRevision_) +
                    L" requested=" + std::to_wstring(settings_.transferCount) +
                    L" available=" + std::to_wstring(pendingImages_.size()) +
                    L" dropping=" + std::to_wstring(pendingImages_.size() - requestedImageCount));
            pendingImages_.resize(requestedImageCount);
        }
    }
    diagnostics::AppendLine(
        L"DS",
        L"Transfer ready from IPC revision=" + std::to_wstring(pendingIpcRevision_) +
            L" imageCount=" + std::to_wstring(pendingImages_.size()) +
            L" xferCount=" + std::to_wstring(settings_.transferCount));
}

bool VirtualTwainDataSource::NotifyTransferReady()
{
    if (transferReadyNotified_)
    {
        diagnostics::AppendLine(L"DS", L"NotifyTransferReady skipped because already notified");
        return true;
    }

    if (entryPoint_.DSM_Entry == nullptr)
    {
        diagnostics::AppendLine(L"DS", L"NotifyTransferReady skipped because DSM_Entry is null");
        return false;
    }

    if (!openOrigin_)
    {
        diagnostics::AppendLine(L"DS", L"NotifyTransferReady skipped because app identity is missing");
        return false;
    }

    TW_IDENTITY sourceIdentity = identity_;
    TW_IDENTITY appIdentity = *openOrigin_;
    const TW_UINT16 rc = entryPoint_.DSM_Entry(
        &sourceIdentity,
        &appIdentity,
        DG_CONTROL,
        DAT_NULL,
        MSG_XFERREADY,
        nullptr);

    diagnostics::AppendLine(
        L"DS",
        L"NotifyTransferReady DAT_NULL/MSG_XFERREADY rc=" + std::to_wstring(rc) +
            L" sourceId=" + std::to_wstring(sourceIdentity.Id) +
            L" appId=" + std::to_wstring(appIdentity.Id));
    if (rc == TWRC_SUCCESS)
    {
        transferReadyNotified_ = true;
        return true;
    }

    return false;
}

void VirtualTwainDataSource::StartTransferReadyWatcher()
{
    if (!transferReadyWatcher_.joinable())
    {
        transferReadyWatcher_ = std::thread(&VirtualTwainDataSource::TransferReadyWatcherLoop, this);
    }

    transferReadyWatcherActive_ = true;
    ++transferReadyWatcherGeneration_;
    diagnostics::AppendLine(
        L"DS",
        L"Transfer ready watcher started generation=" +
            std::to_wstring(transferReadyWatcherGeneration_));
    transferReadyWatcherCv_.notify_all();
}

void VirtualTwainDataSource::StopTransferReadyWatcher()
{
    if (!transferReadyWatcherActive_)
    {
        return;
    }

    transferReadyWatcherActive_ = false;
    ++transferReadyWatcherGeneration_;
    diagnostics::AppendLine(
        L"DS",
        L"Transfer ready watcher stopped generation=" +
            std::to_wstring(transferReadyWatcherGeneration_));
    transferReadyWatcherCv_.notify_all();
}

void VirtualTwainDataSource::TransferReadyWatcherLoop()
{
    ScannerIpcClient client;
    bool loggedState = false;
    std::uint32_t lastRevision = 0;
    bool lastScanRequested = false;
    size_t lastImageCount = 0;
    std::uint64_t loggedGeneration = 0;
    std::uint64_t failureGeneration = 0;
    std::uint32_t consecutiveGetStateFailures = 0;

    diagnostics::AppendLine(L"DS", L"Transfer ready watcher thread entered");

    for (;;)
    {
        std::uint64_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            transferReadyWatcherCv_.wait(lock, [this]
            {
                return transferReadyWatcherStop_ || transferReadyWatcherActive_;
            });

            if (transferReadyWatcherStop_)
            {
                break;
            }

            generation = transferReadyWatcherGeneration_;
            if (state_ != TwainState::SourceEnabled || transferReady_)
            {
                transferReadyWatcherActive_ = false;
                ++transferReadyWatcherGeneration_;
                diagnostics::AppendLine(
                    L"DS",
                    L"Transfer ready watcher paused because state=" + TwainStateName(state_) +
                        L" transferReady=" + std::to_wstring(transferReady_ ? 1U : 0U));
                continue;
            }
        }

        if (failureGeneration != generation)
        {
            failureGeneration = generation;
            consecutiveGetStateFailures = 0;
        }

        ScannerIpcState ipcState{};
        if (!client.TryGetState(ipcState, 150))
        {
            bool shouldNotifyCloseRequest = false;
            TW_ENTRYPOINT closeRequestEntryPoint{};
            TW_IDENTITY closeRequestSourceIdentity{};
            TW_IDENTITY closeRequestAppIdentity{};
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!transferReadyWatcherStop_ &&
                    transferReadyWatcherActive_ &&
                    generation == transferReadyWatcherGeneration_ &&
                    state_ == TwainState::SourceEnabled &&
                    !transferReady_ &&
                    awaitingUiSelection_ &&
                    !closeDsRequest_)
                {
                    ++consecutiveGetStateFailures;
                    diagnostics::AppendLine(
                        L"DS",
                        L"Transfer ready watcher GET_STATE failed while awaiting UI selection failures=" +
                            std::to_wstring(consecutiveGetStateFailures));
                    if (consecutiveGetStateFailures >= kUiStateFailureThreshold)
                    {
                        awaitingUiSelection_ = false;
                        closeDsRequest_ = true;
                        transferReadyWatcherActive_ = false;
                        ++transferReadyWatcherGeneration_;
                        diagnostics::AppendLine(
                            L"DS",
                            L"Transfer ready watcher treating missing UI pipe as canceled session; requesting MSG_CLOSEDSREQ");
                        if (!closeDsRequestNotified_ && entryPoint_.DSM_Entry != nullptr && openOrigin_.has_value())
                        {
                            shouldNotifyCloseRequest = true;
                            closeRequestEntryPoint = entryPoint_;
                            closeRequestSourceIdentity = identity_;
                            closeRequestAppIdentity = *openOrigin_;
                        }
                    }
                }
            }

            if (shouldNotifyCloseRequest)
            {
                const TW_UINT16 rc = closeRequestEntryPoint.DSM_Entry(
                    &closeRequestSourceIdentity,
                    &closeRequestAppIdentity,
                    DG_CONTROL,
                    DAT_NULL,
                    MSG_CLOSEDSREQ,
                    nullptr);

                diagnostics::AppendLine(
                    L"DS",
                    L"NotifyCloseDsRequest DAT_NULL/MSG_CLOSEDSREQ rc=" + std::to_wstring(rc) +
                        L" sourceId=" + std::to_wstring(closeRequestSourceIdentity.Id) +
                        L" appId=" + std::to_wstring(closeRequestAppIdentity.Id));

                if (rc == TWRC_SUCCESS)
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (closeDsRequest_)
                    {
                        closeDsRequestNotified_ = true;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(75));
            continue;
        }

        consecutiveGetStateFailures = 0;

        bool shouldNotify = false;
        std::uint32_t notificationRevision = 0;
        TW_ENTRYPOINT notificationEntryPoint{};
        TW_IDENTITY notificationSourceIdentity{};
        TW_IDENTITY notificationAppIdentity{};

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (transferReadyWatcherStop_ ||
                !transferReadyWatcherActive_ ||
                generation != transferReadyWatcherGeneration_ ||
                state_ != TwainState::SourceEnabled ||
                transferReady_)
            {
                continue;
            }

            ApplyScannerSettingsFromIpc(ipcState);

            if (!loggedState ||
                loggedGeneration != generation ||
                ipcState.revision != lastRevision ||
                ipcState.scanRequested != lastScanRequested ||
                ipcState.selectedImages.size() != lastImageCount)
            {
                diagnostics::AppendLine(
                    L"DS",
                    L"Transfer ready watcher revision=" + std::to_wstring(ipcState.revision) +
                        L" scan=" + std::to_wstring(ipcState.scanRequested ? 1U : 0U) +
                        L" images=" + std::to_wstring(ipcState.selectedImages.size()) +
                        L" pixel=" + ipcState.pixelType +
                        L" xres=" + std::to_wstring(ipcState.xResolution) +
                        L" yres=" + std::to_wstring(ipcState.yResolution));
                loggedState = true;
                lastRevision = ipcState.revision;
                lastScanRequested = ipcState.scanRequested;
                lastImageCount = ipcState.selectedImages.size();
                loggedGeneration = generation;
            }

            if (ipcState.scanRequested && !ipcState.selectedImages.empty())
            {
                CommitTransferReadyFromIpc(std::move(ipcState));
                transferReady_ = true;
                state_ = TwainState::TransferReady;
                transferReadyWatcherActive_ = false;
                ++transferReadyWatcherGeneration_;

                if (entryPoint_.DSM_Entry == nullptr)
                {
                    diagnostics::AppendLine(L"DS", L"NotifyTransferReady skipped because DSM_Entry is null");
                }
                else if (!openOrigin_)
                {
                    diagnostics::AppendLine(L"DS", L"NotifyTransferReady skipped because app identity is missing");
                }
                else if (transferReadyNotified_)
                {
                    diagnostics::AppendLine(L"DS", L"NotifyTransferReady skipped because already notified");
                }
                else
                {
                    shouldNotify = true;
                    notificationRevision = pendingIpcRevision_;
                    notificationEntryPoint = entryPoint_;
                    notificationSourceIdentity = identity_;
                    notificationAppIdentity = *openOrigin_;
                }
            }
        }

        if (shouldNotify)
        {
            const TW_UINT16 rc = notificationEntryPoint.DSM_Entry(
                &notificationSourceIdentity,
                &notificationAppIdentity,
                DG_CONTROL,
                DAT_NULL,
                MSG_XFERREADY,
                nullptr);

            diagnostics::AppendLine(
                L"DS",
                L"NotifyTransferReady DAT_NULL/MSG_XFERREADY rc=" + std::to_wstring(rc) +
                    L" sourceId=" + std::to_wstring(notificationSourceIdentity.Id) +
                    L" appId=" + std::to_wstring(notificationAppIdentity.Id));

            if (rc == TWRC_SUCCESS)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (pendingIpcRevision_ == notificationRevision)
                {
                    transferReadyNotified_ = true;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    diagnostics::AppendLine(L"DS", L"Transfer ready watcher thread exiting");
}

bool VirtualTwainDataSource::BeginUiScanSession(bool shouldShowUi)
{
    if (!shouldShowUi)
    {
        diagnostics::AppendLine(L"DS", L"BeginUiScanSession skipped because shouldShowUi=false");
        return false;
    }

    ScannerIpcClient client;
    ScannerIpcState initialState{};
    initialState.duplexEnabled = settings_.duplexEnabled != FALSE;
    initialState.pixelType = PixelTypeToProtocol(settings_.pixelType);
    initialState.paperSize = PaperSizeToProtocol(settings_.paperSize);
    initialState.xResolution = Fix32WholeOrDefault(settings_.xResolution, 300);
    initialState.yResolution = Fix32WholeOrDefault(settings_.yResolution, 300);

    if (client.BeginScan(initialState, 250))
    {
        diagnostics::AppendLine(L"DS", L"BeginUiScanSession succeeded against existing UI process");
        return true;
    }

    diagnostics::AppendLine(L"DS", L"Initial BEGIN_SCAN failed; attempting to launch UI");

    if (!LaunchScannerUiProcess())
    {
        diagnostics::AppendLine(L"DS", L"LaunchScannerUiProcess failed");
        return false;
    }

    for (int attempt = 0; attempt < 50; ++attempt)
    {
        if (client.BeginScan(initialState, 100))
        {
            diagnostics::AppendLine(
                L"DS",
                L"BeginUiScanSession succeeded after launch on attempt " +
                    std::to_wstring(attempt + 1));
            return true;
        }

        Sleep(100);
    }

    diagnostics::AppendLine(L"DS", L"BeginUiScanSession timed out waiting for UI pipe after launch");
    return false;
}

bool VirtualTwainDataSource::LaunchScannerUiProcess() const
{
    const auto executable = FindUiExecutable();
    if (!executable)
    {
        diagnostics::AppendLine(L"DS", L"LaunchScannerUiProcess could not resolve UI executable");
        return false;
    }

    std::wstring commandLine = L"\"" + *executable + L"\"";
    std::wstring workingDirectory = DirectoryName(*executable);
    diagnostics::AppendLine(
        L"DS",
        L"Launching UI executable=" + *executable +
            L" workingDirectory=" + workingDirectory);

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(
        nullptr,
        commandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
        &startupInfo,
        &processInfo);

    if (!created)
    {
        const DWORD error = GetLastError();
        diagnostics::AppendLine(
            L"DS",
            L"CreateProcessW failed error=" + std::to_wstring(error) +
                L" (" + diagnostics::FormatWindowsError(error) + L")");
        return false;
    }

    diagnostics::AppendLine(
        L"DS",
        L"CreateProcessW succeeded pid=" + std::to_wstring(processInfo.dwProcessId));
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
}

bool VirtualTwainDataSource::TryResolveImageInfoIndex(TW_UINT32& imageIndex) const noexcept
{
    if (state_ == TwainState::Transferring && hasCurrentTransferImage_)
    {
        imageIndex = currentTransferImageIndex_;
        return imageIndex < pendingImages_.size();
    }

    if (state_ == TwainState::TransferReady)
    {
        imageIndex = pendingTransferIndex_;
        return imageIndex < pendingImages_.size();
    }

    return false;
}

bool VirtualTwainDataSource::FillImageInfo(TW_UINT32 imageIndex, pTW_IMAGEINFO imageInfo)
{
    if (imageInfo == nullptr || imageIndex >= pendingImages_.size())
    {
        return false;
    }

    DecodedImageInfo decodedInfo{};
    if (!ImageDib::Probe(pendingImages_[imageIndex], decodedInfo))
    {
        return false;
    }

    std::memset(imageInfo, 0, sizeof(*imageInfo));
    imageInfo->XResolution = settings_.xResolution;
    imageInfo->YResolution = settings_.yResolution;
    imageInfo->ImageWidth = static_cast<TW_INT32>(decodedInfo.width);
    imageInfo->ImageLength = static_cast<TW_INT32>(decodedInfo.height);
    imageInfo->Planar = FALSE;
    imageInfo->PixelType = static_cast<TW_INT16>(settings_.pixelType);
    imageInfo->Compression = TWCP_NONE;

    switch (settings_.pixelType)
    {
    case TWPT_BW:
        imageInfo->SamplesPerPixel = 1;
        imageInfo->BitsPerSample[0] = 1;
        imageInfo->BitsPerPixel = 1;
        break;
    case TWPT_GRAY:
        imageInfo->SamplesPerPixel = 1;
        imageInfo->BitsPerSample[0] = 8;
        imageInfo->BitsPerPixel = 8;
        break;
    case TWPT_RGB:
    default:
        imageInfo->SamplesPerPixel = 3;
        imageInfo->BitsPerSample[0] = 8;
        imageInfo->BitsPerSample[1] = 8;
        imageInfo->BitsPerSample[2] = 8;
        imageInfo->BitsPerPixel = 24;
        break;
    }

    return true;
}

bool VirtualTwainDataSource::EnsureMemoryTransferReady()
{
    if (pendingTransferIndex_ >= pendingImages_.size())
    {
        return false;
    }

    if (memoryTransfer_.active && memoryTransfer_.imageIndex == pendingTransferIndex_)
    {
        return memoryTransfer_.nextRow < memoryTransfer_.image.height;
    }

    ResetMemoryTransfer();
    RasterImage raster{};
    if (!ImageDib::BuildRaster(
            pendingImages_[pendingTransferIndex_],
            settings_.pixelType,
            raster))
    {
        return false;
    }

    memoryTransfer_.active = true;
    memoryTransfer_.imageIndex = pendingTransferIndex_;
    memoryTransfer_.nextRow = 0;
    memoryTransfer_.image = std::move(raster);
    return !memoryTransfer_.image.pixels.empty();
}

void VirtualTwainDataSource::ResetMemoryTransfer() noexcept
{
    memoryTransfer_.active = false;
    memoryTransfer_.imageIndex = 0;
    memoryTransfer_.nextRow = 0;
    memoryTransfer_.image = RasterImage{};
}

void VirtualTwainDataSource::HideScanUiSession()
{
    const std::uint32_t revision = pendingIpcRevision_;
    ScannerIpcClient client;
    const bool hidden = client.HideScanUi(revision, 30);
    diagnostics::AppendLine(
        L"DS",
        L"HideScanUi revision=" + std::to_wstring(revision) +
            L" success=" + std::to_wstring(hidden ? 1U : 0U));
    if (hidden && revision != 0)
    {
        scanUiHiddenForCurrentTransfer_ = true;
    }
}

void VirtualTwainDataSource::HideScanUiIfTransferStarted()
{
    if (pendingIpcRevision_ == 0 || scanUiHiddenForCurrentTransfer_)
    {
        return;
    }

    HideScanUiSession();
}

void VirtualTwainDataSource::ClearTransferProgress() noexcept
{
    pendingImages_.clear();
    pendingIpcRevision_ = 0;
    pendingTransferIndex_ = 0;
    hasCurrentTransferImage_ = false;
    currentTransferImageIndex_ = 0;
    awaitingUiSelection_ = false;
    closeDsRequest_ = false;
    closeDsRequestNotified_ = false;
    scanUiHiddenForCurrentTransfer_ = false;
    ResetMemoryTransfer();
}

void VirtualTwainDataSource::AcknowledgeScanIfComplete()
{
    if (pendingIpcRevision_ == 0)
    {
        return;
    }

    ScannerIpcClient client;
    const bool acknowledged = client.AcknowledgeScan(pendingIpcRevision_, 30);
    diagnostics::AppendLine(
        L"DS",
        L"AcknowledgeScan revision=" + std::to_wstring(pendingIpcRevision_) +
            L" success=" + std::to_wstring(acknowledged ? 1U : 0U));
    pendingIpcRevision_ = 0;
}

void VirtualTwainDataSource::RollbackCanceledUiEnable()
{
    diagnostics::AppendLine(
        L"DS",
        L"RollbackCanceledUiEnable transitioning from state=" + TwainStateName(state_) +
            L" after UI cancellation");
    transferReady_ = false;
    transferReadyNotified_ = false;
    StopTransferReadyWatcher();
    ClearTransferProgress();
    state_ = TwainState::SourceOpened;
}

TW_UINT16 VirtualTwainDataSource::Succeed(TW_UINT16 conditionCode) noexcept
{
    lastStatus_.ConditionCode = conditionCode;
    lastStatus_.Data = 0;
    return TWRC_SUCCESS;
}

TW_UINT16 VirtualTwainDataSource::Fail(TW_UINT16 conditionCode) noexcept
{
    lastStatus_.ConditionCode = conditionCode;
    lastStatus_.Data = 0;
    return TWRC_FAILURE;
}

bool VirtualTwainDataSource::IsAtLeast(TwainState state) const noexcept
{
    return static_cast<TW_UINT16>(state_) >= static_cast<TW_UINT16>(state);
}

TW_IDENTITY VirtualTwainDataSource::BuildSourceIdentity() noexcept
{
    TW_IDENTITY identity{};

    identity.Id = 0;
    identity.Version.MajorNum = 0;
    identity.Version.MinorNum = 2;
    identity.Version.Language = TWLG_ENGLISH_USA;
    identity.Version.Country = TWCY_USA;
    CopyTwainString(identity.Version.Info, sizeof(identity.Version.Info), "mbfTwain Phase 2");

    identity.ProtocolMajor = TWON_PROTOCOLMAJOR;
    identity.ProtocolMinor = TWON_PROTOCOLMINOR;
    identity.SupportedGroups = DG_CONTROL | DG_IMAGE | DF_DS2;

    CopyTwainString(identity.Manufacturer, sizeof(identity.Manufacturer), "MBF");
    CopyTwainString(identity.ProductFamily, sizeof(identity.ProductFamily), "Virtual Scanner");
    CopyTwainString(identity.ProductName, sizeof(identity.ProductName), "mbf Virtual TWAIN Scanner");

    return identity;
}

TW_FIX32 VirtualTwainDataSource::MakeFix32(TW_INT16 whole, TW_UINT16 fraction) noexcept
{
    TW_FIX32 value{};
    value.Whole = whole;
    value.Frac = fraction;
    return value;
}

} // namespace mbf::twain

extern "C" TW_UINT16 TW_CALLINGSTYLE DS_Entry(
    pTW_IDENTITY origin,
    TW_UINT32 dataGroup,
    TW_UINT16 dataArgumentType,
    TW_UINT16 message,
    TW_MEMREF data)
{
    return mbf::twain::VirtualTwainDataSource::Instance().Entry(
        origin,
        dataGroup,
        dataArgumentType,
        message,
        data);
}
