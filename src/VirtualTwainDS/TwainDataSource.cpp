#include "TwainDataSource.h"

#include "DiagnosticsLog.h"
#include "ImageDib.h"
#include "ScannerIpcClient.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

constexpr std::uint32_t kUiStateFailureThreshold = 3;

int g_moduleAnchor = 0;

std::wstring ModulePath(HMODULE module)
{
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
        {
            return L"(unknown)";
        }
        if (length < buffer.size() - 1)
        {
            return std::wstring(buffer.data(), length);
        }

        buffer.resize(buffer.size() * 2U);
    }
}

std::wstring CurrentModulePath()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&g_moduleAnchor),
            &module))
    {
        return L"(unknown)";
    }

    return ModulePath(module);
}

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

void SetExtInfoUnsupported(TW_INFO& info) noexcept
{
    info.ItemType = TWTY_UINT16;
    info.NumItems = 0;
    info.ReturnCode = TWRC_INFONOTSUPPORTED;
    info.Item = 0;
}

void SetExtInfoUInt16(TW_INFO& info, TW_UINT16 value) noexcept
{
    info.ItemType = TWTY_UINT16;
    info.NumItems = 1;
    info.ReturnCode = TWRC_SUCCESS;
    info.Item = value;
}

void SetExtInfoUInt32(TW_INFO& info, TW_UINT32 value) noexcept
{
    info.ItemType = TWTY_UINT32;
    info.NumItems = 1;
    info.ReturnCode = TWRC_SUCCESS;
    info.Item = value;
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
    case DAT_EXTIMAGEINFO:
        return L"DAT_EXTIMAGEINFO";
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
    diagnostics::AppendLine(
        L"DS",
        L"VirtualTwainDataSource initialized module=" + CurrentModulePath() +
            L" logPath=" + diagnostics::LogFilePath());
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
    std::unique_lock<std::mutex> lock(mutex_);
    const std::wstring triplet = DescribeTriplet(dataGroup, dataArgumentType, message, data);
    diagnostics::AppendLine(
        L"DS",
        L"Entry begin state=" + TwainStateName(state_) + L" " + triplet);

    TW_UINT16 result = TWRC_FAILURE;

    if (dataGroup == DG_IMAGE)
    {
        result = HandleImage(dataArgumentType, message, data, lock);
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
            result = HandleIdentity(message, origin, data, lock);
            break;
        case DAT_CAPABILITY:
            result = HandleCapability(message, data);
            break;
        case DAT_USERINTERFACE:
            result = HandleUserInterface(message, data, lock);
            break;
        case DAT_EVENT:
            result = HandleEvent(message, data, lock);
            break;
        case DAT_PENDINGXFERS:
            result = HandlePendingTransfers(message, data, lock);
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

    if (!lock.owns_lock())
    {
        lock.lock();
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
    TW_MEMREF data,
    std::unique_lock<std::mutex>& lock)
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

        if (transferSession_.HasRevision())
        {
            AcknowledgeScanIfComplete(lock);
        }
        else
        {
            HideScanUiSession(lock);
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

TW_UINT16 VirtualTwainDataSource::HandleUserInterface(
    TW_UINT16 message,
    TW_MEMREF data,
    std::unique_lock<std::mutex>& lock)
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

        const bool forceUi = EnvironmentFlagEnabled(L"MBF_TWAIN_FORCE_UI");
        const bool shouldShowUi =
            message == MSG_ENABLEDSUIONLY ||
            userInterface->ShowUI != FALSE ||
            forceUi;

        ScannerIpcState initialState{};
        if (shouldShowUi)
        {
            initialState.duplexEnabled = capabilities_.DuplexEnabled() != FALSE;
            initialState.pixelType = PixelTypeToProtocol(capabilities_.PixelType());
            initialState.paperSize = PaperSizeToProtocol(capabilities_.PaperSize());
            initialState.xResolution = Fix32WholeOrDefault(capabilities_.XResolution(), 300);
            initialState.yResolution = Fix32WholeOrDefault(capabilities_.YResolution(), 300);
        }

        diagnostics::AppendLine(
            L"DS",
            L"HandleUserInterface enable shouldShowUi=" + std::to_wstring(shouldShowUi ? 1U : 0U) +
                L" showUI=" + std::to_wstring(userInterface->ShowUI != FALSE ? 1U : 0U) +
                L" forceUi=" +
                std::to_wstring(forceUi ? 1U : 0U));

        transferReady_ = false;
        transferReadyNotified_ = false;
        ipcSession_.ResetUiFlow();
        ClearTransferProgress();
        state_ = TwainState::SourceEnabled;

        if (shouldShowUi)
        {
            lock.unlock();
            const bool scanSessionStarted = BeginUiScanSession(initialState);
            lock.lock();

            if (state_ != TwainState::SourceEnabled)
            {
                diagnostics::AppendLine(
                    L"DS",
                    L"BeginUiScanSession completed after state changed to " + TwainStateName(state_));
                return Fail(TWCC_SEQERROR);
            }

            if (!scanSessionStarted)
            {
                state_ = TwainState::SourceOpened;
                diagnostics::AppendLine(
                    L"DS",
                    L"BeginUiScanSession failed during enable; reverting to SourceOpened");
                return Fail(TWCC_OPERATIONERROR);
            }
        }

        if (!shouldShowUi)
        {
            const bool transferReady = RefreshTransferReadyFromIpc(lock);
            if (state_ != TwainState::SourceEnabled)
            {
                diagnostics::AppendLine(
                    L"DS",
                    L"RefreshTransferReadyFromIpc completed after state changed to " + TwainStateName(state_));
                return Fail(TWCC_SEQERROR);
            }

            if (transferReady)
            {
                transferReady_ = true;
                state_ = TwainState::TransferReady;
                NotifyTransferReady(lock);
            }
        }
        else if (shouldShowUi && message == MSG_ENABLEDS)
        {
            ipcSession_.BeginAwaitingUiSelection();
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
        if (transferSession_.HasRevision())
        {
            AcknowledgeScanIfComplete(lock);
        }
        else
        {
            HideScanUiSession(lock);
        }
        ClearTransferProgress();
        state_ = TwainState::SourceOpened;
        return Succeed();

    default:
        return Fail(TWCC_BADPROTOCOL);
    }
}

TW_UINT16 VirtualTwainDataSource::HandleEvent(
    TW_UINT16 message,
    TW_MEMREF data,
    std::unique_lock<std::mutex>& lock)
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

    if (!transferReady_ && !ipcSession_.CloseDsRequested())
    {
        transferReady_ = RefreshTransferReadyFromIpc(lock);
        if (!IsAtLeast(TwainState::SourceEnabled))
        {
            diagnostics::AppendLine(
                L"DS",
                L"HandleEvent refresh completed after state changed to " + TwainStateName(state_));
            return Fail(TWCC_SEQERROR);
        }
    }

    if (ipcSession_.CloseDsRequested())
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

TW_UINT16 VirtualTwainDataSource::HandlePendingTransfers(
    TW_UINT16 message,
    TW_MEMREF data,
    std::unique_lock<std::mutex>& lock)
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

        CompleteTransferIfUiStopped(lock);
        pendingTransfers->Count = transferSession_.RemainingImageCountForTwain();
        pendingTransfers->EOJ = 0;
        return Succeed();
    }

    case MSG_ENDXFER:
    {
        if (state_ != TwainState::Transferring && state_ != TwainState::TransferReady)
        {
            return Fail(TWCC_SEQERROR);
        }

        CompleteTransferIfUiStopped(lock);
        const TW_UINT32 remaining = transferSession_.RemainingImageCount();
        pendingTransfers->Count = transferSession_.RemainingImageCountForTwain();
        pendingTransfers->EOJ = 0;

        if (remaining > 0)
        {
            transferReady_ = true;
            transferReadyNotified_ = false;
            transferSession_.ClearCurrentImage();
            state_ = TwainState::TransferReady;
        }
        else
        {
            transferReady_ = false;
            transferReadyNotified_ = false;
            transferSession_.ClearCurrentImage();
            ResetMemoryTransfer();
            state_ = TwainState::SourceEnabled;
            AcknowledgeScanIfComplete(lock);
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
        ipcSession_.ResetUiFlow();
        transferSession_.DiscardImages();
        state_ = resetState;
        AcknowledgeScanIfComplete(lock);
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
    TW_MEMREF data,
    std::unique_lock<std::mutex>& lock)
{
    switch (dataArgumentType)
    {
    case DAT_IMAGEINFO:
        return HandleImageInfo(message, data, lock);
    case DAT_EXTIMAGEINFO:
        return HandleExtendedImageInfo(message, data);
    case DAT_IMAGENATIVEXFER:
        return HandleImageNativeTransfer(message, data, lock);
    case DAT_IMAGEMEMXFER:
        return HandleImageMemoryTransfer(message, data, lock);
    default:
        return Fail(TWCC_BADPROTOCOL);
    }
}

TW_UINT16 VirtualTwainDataSource::HandleImageInfo(
    TW_UINT16 message,
    TW_MEMREF data,
    std::unique_lock<std::mutex>& lock)
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
        !FillImageInfo(imageIndex, imageInfo, lock))
    {
        return Fail(TWCC_OPERATIONERROR);
    }

    return Succeed();
}

TW_UINT16 VirtualTwainDataSource::HandleExtendedImageInfo(TW_UINT16 message, TW_MEMREF data)
{
    if (message != MSG_GET)
    {
        return Fail(TWCC_BADPROTOCOL);
    }
    if (!IsAtLeast(TwainState::TransferReady))
    {
        return Fail(TWCC_SEQERROR);
    }

    auto* extendedImageInfo = static_cast<pTW_EXTIMAGEINFO>(data);
    if (extendedImageInfo == nullptr)
    {
        return Fail(TWCC_BADVALUE);
    }

    TW_UINT32 imageIndex = 0;
    if (!TryResolveImageInfoIndex(imageIndex) ||
        !FillExtendedImageInfo(imageIndex, extendedImageInfo))
    {
        return Fail(TWCC_OPERATIONERROR);
    }

    return Succeed();
}

TW_UINT16 VirtualTwainDataSource::HandleImageNativeTransfer(
    TW_UINT16 message,
    TW_MEMREF data,
    std::unique_lock<std::mutex>& lock)
{
    if (message != MSG_GET)
    {
        return Fail(TWCC_BADPROTOCOL);
    }
    if (state_ != TwainState::TransferReady)
    {
        return Fail(TWCC_SEQERROR);
    }
    if (!transferSession_.HasPendingImage())
    {
        return Fail(TWCC_SEQERROR);
    }

    auto* outputHandle = static_cast<TW_HANDLE*>(data);
    if (outputHandle == nullptr)
    {
        return Fail(TWCC_BADVALUE);
    }

    if (CompleteTransferIfUiStopped(lock))
    {
        *outputHandle = nullptr;
        lastStatus_.ConditionCode = TWCC_SUCCESS;
        lastStatus_.Data = 0;
        return TWRC_CANCEL;
    }

    DecodedImageInfo decodedInfo{};
    const TW_UINT32 imageIndex = transferSession_.PendingImageIndex();
    const ScannerIpcImage selectedImage = transferSession_.PendingImage();
    const TW_UINT16 pixelType = capabilities_.PixelType();
    const std::uint32_t xResolution = Fix32WholeOrDefault(capabilities_.XResolution(), 300);
    const std::uint32_t yResolution = Fix32WholeOrDefault(capabilities_.YResolution(), 300);

    ReportTransferProgress(lock, imageIndex);
    if (state_ != TwainState::TransferReady ||
        transferSession_.PendingImageIndex() != imageIndex ||
        !transferSession_.MatchesImage(imageIndex, selectedImage) ||
        capabilities_.PixelType() != pixelType ||
        Fix32WholeOrDefault(capabilities_.XResolution(), 300) != xResolution ||
        Fix32WholeOrDefault(capabilities_.YResolution(), 300) != yResolution)
    {
        return Fail(TWCC_SEQERROR);
    }

    lock.unlock();
    TW_HANDLE dib = ImageDib::BuildNativeDib(
        selectedImage.path,
        pixelType,
        xResolution,
        yResolution,
        selectedImage.rotationDegrees,
        decodedInfo);
    lock.lock();

    if (dib == nullptr)
    {
        return Fail(TWCC_OPERATIONERROR);
    }

    if (state_ != TwainState::TransferReady ||
        transferSession_.PendingImageIndex() != imageIndex ||
        !transferSession_.MatchesImage(imageIndex, selectedImage) ||
        capabilities_.PixelType() != pixelType ||
        Fix32WholeOrDefault(capabilities_.XResolution(), 300) != xResolution ||
        Fix32WholeOrDefault(capabilities_.YResolution(), 300) != yResolution)
    {
        GlobalFree(dib);
        return Fail(TWCC_SEQERROR);
    }

    *outputHandle = dib;
    transferSession_.AdvancePendingImage();
    transferSession_.MarkCurrentImage(imageIndex);
    state_ = TwainState::Transferring;
    transferReady_ = transferSession_.HasPendingImage();
    ReportTransferProgress(lock, imageIndex + 1U);
    ApplyTransferBufferDelay(lock);
    LimitTransferToCurrentImageIfUiStopped(lock, imageIndex);
    HideScanUiIfTransferStarted(lock);
    lastStatus_.ConditionCode = TWCC_SUCCESS;
    lastStatus_.Data = 0;
    return TWRC_XFERDONE;
}

TW_UINT16 VirtualTwainDataSource::HandleImageMemoryTransfer(
    TW_UINT16 message,
    TW_MEMREF data,
    std::unique_lock<std::mutex>& lock)
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

    if (CompleteTransferIfUiStopped(lock))
    {
        transfer->Compression = TWCP_NONE;
        transfer->BytesPerRow = 0;
        transfer->Columns = 0;
        transfer->Rows = 0;
        transfer->XOffset = 0;
        transfer->YOffset = 0;
        transfer->BytesWritten = 0;
        lastStatus_.ConditionCode = TWCC_SUCCESS;
        lastStatus_.Data = 0;
        return TWRC_CANCEL;
    }

    if (!EnsureMemoryTransferReady(lock))
    {
        return Fail(TWCC_OPERATIONERROR);
    }

    if (transferSession_.MemoryNextRow() == 0)
    {
        ReportTransferProgress(lock, transferSession_.PendingImageIndex());
        if (state_ != TwainState::TransferReady && state_ != TwainState::Transferring)
        {
            return Fail(TWCC_SEQERROR);
        }
        if (!transferSession_.HasMemoryForPendingImage())
        {
            return Fail(TWCC_OPERATIONERROR);
        }
    }

    BYTE* destination = nullptr;
    bool unlockHandle = false;
    if (!LockTwainMemory(transfer->Memory, destination, unlockHandle))
    {
        return Fail(TWCC_BADVALUE);
    }

    const RasterImage& memoryImage = transferSession_.MemoryImage();
    const TW_UINT32 bytesPerRow = memoryImage.bytesPerRow;
    if (bytesPerRow == 0 || transfer->Memory.Length < bytesPerRow)
    {
        UnlockTwainMemory(transfer->Memory, unlockHandle);
        return Fail(TWCC_BADVALUE);
    }

    const TW_UINT32 memoryNextRow = transferSession_.MemoryNextRow();
    const TW_UINT32 rowsRemaining = memoryImage.height - memoryNextRow;
    const TW_UINT32 rowsThatFit = transfer->Memory.Length / bytesPerRow;
    const TW_UINT32 rowsToWrite = (std::min)(rowsRemaining, rowsThatFit);
    const TW_UINT32 bytesToWrite = rowsToWrite * bytesPerRow;
    if (rowsToWrite == 0 || bytesToWrite == 0)
    {
        UnlockTwainMemory(transfer->Memory, unlockHandle);
        return Fail(TWCC_BADVALUE);
    }

    const BYTE* source = memoryImage.pixels.data() +
        (static_cast<size_t>(memoryNextRow) * bytesPerRow);
    std::memcpy(destination, source, bytesToWrite);
    UnlockTwainMemory(transfer->Memory, unlockHandle);

    transfer->Compression = TWCP_NONE;
    transfer->BytesPerRow = bytesPerRow;
    transfer->Columns = memoryImage.width;
    transfer->Rows = rowsToWrite;
    transfer->XOffset = 0;
    transfer->YOffset = memoryNextRow;
    transfer->BytesWritten = bytesToWrite;

    transferSession_.AdvanceMemoryRows(rowsToWrite);
    transferSession_.MarkCurrentImage(transferSession_.MemoryImageIndex());
    state_ = TwainState::Transferring;
    lastStatus_.ConditionCode = TWCC_SUCCESS;
    lastStatus_.Data = 0;

    if (transferSession_.MemoryComplete())
    {
        const TW_UINT32 completedImageCount = transferSession_.MemoryImageIndex() + 1U;
        transferSession_.AdvancePendingImage();
        transferReady_ = transferSession_.HasPendingImage();
        ResetMemoryTransfer();
        ReportTransferProgress(lock, completedImageCount);
        ApplyTransferBufferDelay(lock);
        LimitTransferToCurrentImageIfUiStopped(lock, completedImageCount - 1U);
        HideScanUiIfTransferStarted(lock);
        return TWRC_XFERDONE;
    }

    ApplyTransferBufferDelay(lock);
    LimitTransferToCurrentImageIfUiStopped(lock, transferSession_.MemoryImageIndex());
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
    return CompleteCapabilityResult(capabilities_.Get(message, capability));
}

TW_UINT16 VirtualTwainDataSource::SetCapability(pTW_CAPABILITY capability)
{
    return CompleteCapabilityResult(capabilities_.Set(capability));
}

TW_UINT16 VirtualTwainDataSource::ResetCapability(pTW_CAPABILITY capability)
{
    return CompleteCapabilityResult(capabilities_.Reset(capability));
}

TW_UINT16 VirtualTwainDataSource::QueryCapabilitySupport(pTW_CAPABILITY capability)
{
    return CompleteCapabilityResult(capabilities_.QuerySupport(capability));
}

TW_UINT16 VirtualTwainDataSource::ResetAllCapabilities() noexcept
{
    return CompleteCapabilityResult(capabilities_.ResetAll());
}

TW_UINT16 VirtualTwainDataSource::ResetCapabilityValue(TW_UINT16 capability) noexcept
{
    return CompleteCapabilityResult(capabilities_.ResetValue(capability));
}
bool VirtualTwainDataSource::RefreshTransferReadyFromIpc(std::unique_lock<std::mutex>& lock)
{
    ScannerIpcClient client;
    ScannerIpcState ipcState{};
    lock.unlock();
    const bool gotState = client.TryGetState(ipcState, 30);
    if (!gotState)
    {
        diagnostics::AppendLine(L"DS", L"RefreshTransferReadyFromIpc GET_STATE failed");
        lock.lock();
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
            L" yres=" + std::to_wstring(ipcState.yResolution) +
            L" transferDelayMs=" + std::to_wstring(ipcState.transferBufferDelayMilliseconds));

    lock.lock();
    if (!IsAtLeast(TwainState::SourceEnabled))
    {
        diagnostics::AppendLine(
            L"DS",
            L"RefreshTransferReadyFromIpc discarded because state=" + TwainStateName(state_));
        return false;
    }

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
    TW_UINT16 mappedPixelType = capabilities_.PixelType();
    if (TryMapPixelType(ipcState.pixelType, mappedPixelType))
    {
        capabilities_.SetPixelTypeIfSupported(mappedPixelType);
    }

    TW_UINT16 mappedPaperSize = capabilities_.PaperSize();
    if (TryMapPaperSize(ipcState.paperSize, mappedPaperSize))
    {
        capabilities_.SetPaperSizeIfSupported(mappedPaperSize);
    }

    const auto xResolution = MakeFix32(static_cast<TW_INT16>(ipcState.xResolution));
    capabilities_.SetXResolutionIfSupported(xResolution);

    const auto yResolution = MakeFix32(static_cast<TW_INT16>(ipcState.yResolution));
    capabilities_.SetYResolutionIfSupported(yResolution);

    capabilities_.SetDuplexEnabled(ipcState.duplexEnabled);
}

void VirtualTwainDataSource::CommitTransferReadyFromIpc(ScannerIpcState&& ipcState)
{
    const std::uint32_t revision = ipcState.revision;
    const std::uint32_t transferBufferDelayMilliseconds = ipcState.transferBufferDelayMilliseconds;
    ipcSession_.ResetUiFlow();
    transferReadyNotified_ = false;
    transferSession_.Begin(
        revision,
        std::move(ipcState.selectedImages),
        transferBufferDelayMilliseconds);
    if (capabilities_.TransferCount() > 0)
    {
        const size_t requestedImageCount = static_cast<size_t>(capabilities_.TransferCount());
        if (transferSession_.ImageCount() > requestedImageCount)
        {
            diagnostics::AppendLine(
                L"DS",
                L"CAP_XFERCOUNT truncating scan revision=" + std::to_wstring(transferSession_.Revision()) +
                    L" requested=" + std::to_wstring(capabilities_.TransferCount()) +
                    L" available=" + std::to_wstring(transferSession_.ImageCount()) +
                    L" dropping=" + std::to_wstring(transferSession_.ImageCount() - requestedImageCount));
            transferSession_.LimitImages(requestedImageCount);
        }
    }
    diagnostics::AppendLine(
        L"DS",
        L"Transfer ready from IPC revision=" + std::to_wstring(transferSession_.Revision()) +
            L" imageCount=" + std::to_wstring(transferSession_.ImageCount()) +
            L" transferDelayMs=" + std::to_wstring(transferSession_.TransferBufferDelayMilliseconds()) +
            L" xferCount=" + std::to_wstring(capabilities_.TransferCount()));
}

bool VirtualTwainDataSource::NotifyTransferReady(std::unique_lock<std::mutex>& lock)
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
    const TW_ENTRYPOINT entryPoint = entryPoint_;
    const std::uint32_t revision = transferSession_.Revision();

    lock.unlock();
    const TW_UINT16 rc = entryPoint.DSM_Entry(
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
    lock.lock();
    if (rc == TWRC_SUCCESS)
    {
        if (transferSession_.Revision() == revision)
        {
            transferReadyNotified_ = true;
        }
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
                    ipcSession_.AwaitingUiSelection() &&
                    !ipcSession_.CloseDsRequested())
                {
                    ++consecutiveGetStateFailures;
                    diagnostics::AppendLine(
                        L"DS",
                        L"Transfer ready watcher GET_STATE failed while awaiting UI selection failures=" +
                            std::to_wstring(consecutiveGetStateFailures));
                    if (consecutiveGetStateFailures >= kUiStateFailureThreshold)
                    {
                        ipcSession_.RequestCloseDs();
                        transferReadyWatcherActive_ = false;
                        ++transferReadyWatcherGeneration_;
                        diagnostics::AppendLine(
                            L"DS",
                            L"Transfer ready watcher treating missing UI pipe as canceled session; requesting MSG_CLOSEDSREQ");
                        if (!ipcSession_.CloseDsRequestNotified() && entryPoint_.DSM_Entry != nullptr && openOrigin_.has_value())
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
                    if (ipcSession_.CloseDsRequested())
                    {
                        ipcSession_.MarkCloseDsRequestNotified();
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
                    notificationRevision = transferSession_.Revision();
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
                if (transferSession_.Revision() == notificationRevision)
                {
                    transferReadyNotified_ = true;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    diagnostics::AppendLine(L"DS", L"Transfer ready watcher thread exiting");
}

bool VirtualTwainDataSource::BeginUiScanSession(const ScannerIpcState& initialState)
{
    ScannerIpcClient client;

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
    if (state_ == TwainState::Transferring && transferSession_.HasCurrentImage())
    {
        imageIndex = transferSession_.CurrentImageIndex();
        return imageIndex < transferSession_.ImageCount();
    }

    if (state_ == TwainState::TransferReady)
    {
        imageIndex = transferSession_.PendingImageIndex();
        return imageIndex < transferSession_.ImageCount();
    }

    return false;
}

bool VirtualTwainDataSource::FillImageInfo(
    TW_UINT32 imageIndex,
    pTW_IMAGEINFO imageInfo,
    std::unique_lock<std::mutex>& lock)
{
    if (imageInfo == nullptr || imageIndex >= transferSession_.ImageCount())
    {
        return false;
    }

    const ScannerIpcImage selectedImage = transferSession_.ImageAt(imageIndex);
    const TW_FIX32 xResolution = capabilities_.XResolution();
    const TW_FIX32 yResolution = capabilities_.YResolution();
    const TW_UINT16 pixelType = capabilities_.PixelType();

    DecodedImageInfo decodedInfo{};
    lock.unlock();
    if (!ImageDib::Probe(selectedImage.path, decodedInfo, selectedImage.rotationDegrees))
    {
        lock.lock();
        return false;
    }
    lock.lock();

    if (!transferSession_.MatchesImage(imageIndex, selectedImage) ||
        capabilities_.XResolution().Whole != xResolution.Whole ||
        capabilities_.XResolution().Frac != xResolution.Frac ||
        capabilities_.YResolution().Whole != yResolution.Whole ||
        capabilities_.YResolution().Frac != yResolution.Frac ||
        capabilities_.PixelType() != pixelType)
    {
        return false;
    }

    std::memset(imageInfo, 0, sizeof(*imageInfo));
    imageInfo->XResolution = xResolution;
    imageInfo->YResolution = yResolution;
    imageInfo->ImageWidth = static_cast<TW_INT32>(decodedInfo.width);
    imageInfo->ImageLength = static_cast<TW_INT32>(decodedInfo.height);
    imageInfo->Planar = FALSE;
    imageInfo->PixelType = static_cast<TW_INT16>(pixelType);
    imageInfo->Compression = TWCP_NONE;

    switch (pixelType)
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

bool VirtualTwainDataSource::FillExtendedImageInfo(
    TW_UINT32 imageIndex,
    pTW_EXTIMAGEINFO extendedImageInfo) const noexcept
{
    if (extendedImageInfo == nullptr || imageIndex >= transferSession_.ImageCount())
    {
        return false;
    }

    const bool duplex = capabilities_.DuplexEnabled() != FALSE;
    const TW_UINT32 imageCount = static_cast<TW_UINT32>(transferSession_.ImageCount());
    const TW_UINT32 paperNumber = duplex ? (imageIndex / 2U) + 1U : imageIndex + 1U;
    const TW_UINT32 paperCount = duplex ? (imageCount + 1U) / 2U : imageCount;
    const TW_UINT16 pageSide = duplex && (imageIndex % 2U) == 1U ? TWCS_BOTTOM : TWCS_TOP;

    for (TW_UINT32 index = 0; index < extendedImageInfo->NumInfos; ++index)
    {
        TW_INFO& info = extendedImageInfo->Info[index];
        switch (info.InfoID)
        {
        case TWEI_DOCUMENTNUMBER:
            SetExtInfoUInt32(info, 1);
            break;
        case TWEI_PAGENUMBER:
            SetExtInfoUInt32(info, paperNumber);
            break;
        case TWEI_CAMERA:
        case TWEI_PAGESIDE:
            SetExtInfoUInt16(info, pageSide);
            break;
        case TWEI_FRAMENUMBER:
            SetExtInfoUInt32(info, imageIndex + 1U);
            break;
        case TWEI_PAPERCOUNT:
            SetExtInfoUInt32(info, paperCount);
            break;
        default:
            SetExtInfoUnsupported(info);
            break;
        }
    }

    return true;
}

bool VirtualTwainDataSource::EnsureMemoryTransferReady(std::unique_lock<std::mutex>& lock)
{
    if (!transferSession_.HasPendingImage())
    {
        return false;
    }

    if (transferSession_.HasMemoryForPendingImage())
    {
        return transferSession_.MemoryHasRowsRemaining();
    }

    ResetMemoryTransfer();
    RasterImage raster{};
    const TW_UINT32 imageIndex = transferSession_.PendingImageIndex();
    const ScannerIpcImage selectedImage = transferSession_.PendingImage();
    const TW_UINT16 pixelType = capabilities_.PixelType();

    lock.unlock();
    if (!ImageDib::BuildRaster(
            selectedImage.path,
            pixelType,
            raster,
            selectedImage.rotationDegrees))
    {
        lock.lock();
        return false;
    }
    lock.lock();

    if ((state_ != TwainState::TransferReady && state_ != TwainState::Transferring) ||
        transferSession_.PendingImageIndex() != imageIndex ||
        !transferSession_.MatchesImage(imageIndex, selectedImage) ||
        capabilities_.PixelType() != pixelType)
    {
        return false;
    }

    transferSession_.BeginMemory(imageIndex, std::move(raster));
    return !transferSession_.MemoryImage().pixels.empty();
}

void VirtualTwainDataSource::ResetMemoryTransfer() noexcept
{
    transferSession_.ResetMemory();
}

bool VirtualTwainDataSource::IsUiStopRequested(std::unique_lock<std::mutex>& lock)
{
    if (!transferSession_.HasRevision())
    {
        return false;
    }

    const std::uint32_t revision = transferSession_.Revision();
    ScannerIpcState ipcState{};
    ScannerIpcClient client;
    lock.unlock();
    const bool gotState = client.TryGetState(ipcState, 30);
    lock.lock();

    if (!gotState ||
        !transferSession_.HasRevision() ||
        transferSession_.Revision() != revision)
    {
        return false;
    }

    return !ipcState.scanRequested;
}

bool VirtualTwainDataSource::CompleteTransferIfUiStopped(std::unique_lock<std::mutex>& lock)
{
    if (!IsUiStopRequested(lock))
    {
        return false;
    }

    diagnostics::AppendLine(
        L"DS",
        L"UI requested scan stop; completing transfer session revision=" +
            std::to_wstring(transferSession_.Revision()));
    transferReady_ = false;
    transferReadyNotified_ = false;
    transferSession_.DiscardImages();
    ResetMemoryTransfer();
    return true;
}

bool VirtualTwainDataSource::LimitTransferToCurrentImageIfUiStopped(
    std::unique_lock<std::mutex>& lock,
    TW_UINT32 imageIndex)
{
    if (!IsUiStopRequested(lock))
    {
        return false;
    }

    const size_t keepImageCount = (std::min)(
        static_cast<size_t>(imageIndex) + 1U,
        transferSession_.ImageCount());
    diagnostics::AppendLine(
        L"DS",
        L"UI requested scan stop; trimming remaining images revision=" +
            std::to_wstring(transferSession_.Revision()) +
            L" keep=" + std::to_wstring(keepImageCount) +
            L" available=" + std::to_wstring(transferSession_.ImageCount()));
    transferSession_.LimitImages(keepImageCount);
    transferReady_ = transferSession_.HasPendingImage();
    transferReadyNotified_ = false;
    return true;
}

void VirtualTwainDataSource::ReportTransferProgress(
    std::unique_lock<std::mutex>& lock,
    TW_UINT32 completedImages)
{
    if (!transferSession_.HasRevision() || transferSession_.ImageCount() == 0)
    {
        return;
    }

    const std::uint32_t revision = transferSession_.Revision();
    const TW_UINT32 totalImages = static_cast<TW_UINT32>(
        (std::min)(transferSession_.ImageCount(), static_cast<size_t>(0xffffffffU)));
    const TW_UINT32 boundedCompleted = (std::min)(completedImages, totalImages);

    ScannerIpcClient client;
    lock.unlock();
    const bool reported = client.ReportTransferProgress(revision, boundedCompleted, totalImages, 30);
    diagnostics::AppendLine(
        L"DS",
        L"TransferProgress revision=" + std::to_wstring(revision) +
            L" completed=" + std::to_wstring(boundedCompleted) +
            L" total=" + std::to_wstring(totalImages) +
            L" success=" + std::to_wstring(reported ? 1U : 0U));
    lock.lock();
}

void VirtualTwainDataSource::ApplyTransferBufferDelay(std::unique_lock<std::mutex>& lock)
{
    const std::uint32_t delayMilliseconds = transferSession_.TransferBufferDelayMilliseconds();
    if (delayMilliseconds == 0)
    {
        return;
    }

    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(delayMilliseconds));
    lock.lock();
}

void VirtualTwainDataSource::HideScanUiSession(std::unique_lock<std::mutex>& lock)
{
    const std::uint32_t revision = transferSession_.Revision();
    ScannerIpcClient client;
    lock.unlock();
    const bool hidden = client.HideScanUi(revision, 30);
    diagnostics::AppendLine(
        L"DS",
        L"HideScanUi revision=" + std::to_wstring(revision) +
            L" success=" + std::to_wstring(hidden ? 1U : 0U));
    lock.lock();
    if (hidden && revision != 0 && transferSession_.Revision() == revision)
    {
        transferSession_.MarkScanUiHidden();
    }
}

void VirtualTwainDataSource::HideScanUiIfTransferStarted(std::unique_lock<std::mutex>& lock)
{
    if (!transferSession_.HasRevision() || transferSession_.ScanUiHidden())
    {
        return;
    }

    HideScanUiSession(lock);
}

void VirtualTwainDataSource::ClearTransferProgress() noexcept
{
    ipcSession_.ResetUiFlow();
    transferSession_.Clear();
}

void VirtualTwainDataSource::AcknowledgeScanIfComplete(std::unique_lock<std::mutex>& lock)
{
    if (!transferSession_.HasRevision())
    {
        return;
    }

    const std::uint32_t revision = transferSession_.TakeRevision();
    ScannerIpcClient client;
    lock.unlock();
    const bool acknowledged = client.AcknowledgeScan(revision, 30);
    diagnostics::AppendLine(
        L"DS",
        L"AcknowledgeScan revision=" + std::to_wstring(revision) +
            L" success=" + std::to_wstring(acknowledged ? 1U : 0U));
    lock.lock();
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

TW_UINT16 VirtualTwainDataSource::CompleteCapabilityResult(CapabilityResult result) noexcept
{
    return result.returnCode == TWRC_SUCCESS
        ? Succeed(result.conditionCode)
        : Fail(result.conditionCode);
}

bool VirtualTwainDataSource::IsAtLeast(TwainState state) const noexcept
{
    return static_cast<TW_UINT16>(state_) >= static_cast<TW_UINT16>(state);
}

TW_IDENTITY VirtualTwainDataSource::BuildSourceIdentity() noexcept
{
    TW_IDENTITY identity{};

    identity.Id = 0;
    identity.Version.MajorNum = 1;
    identity.Version.MinorNum = 0;
    identity.Version.Language = TWLG_ENGLISH_USA;
    identity.Version.Country = TWCY_USA;
    CopyTwainString(identity.Version.Info, sizeof(identity.Version.Info), "mbfTwain 1.0.3");

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
