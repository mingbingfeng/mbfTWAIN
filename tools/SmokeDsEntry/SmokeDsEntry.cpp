#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <cstring>

#include "twain.h"

namespace
{

using DsEntry = TW_UINT16(TW_CALLINGSTYLE*)(
    pTW_IDENTITY origin,
    TW_UINT32 dataGroup,
    TW_UINT16 dataArgumentType,
    TW_UINT16 message,
    TW_MEMREF data);

std::atomic_bool g_sawXferReadyCallback{false};
std::atomic<TW_UINT32> g_xferReadySourceId{0};
std::atomic<TW_UINT32> g_xferReadyAppId{0};

TW_UINT16 TW_CALLINGSTYLE SmokeDsmEntry(
    pTW_IDENTITY origin,
    pTW_IDENTITY destination,
    TW_UINT32 dataGroup,
    TW_UINT16 dataArgumentType,
    TW_UINT16 message,
    TW_MEMREF)
{
    if (origin != nullptr &&
        destination != nullptr &&
        dataGroup == DG_CONTROL &&
        dataArgumentType == DAT_NULL &&
        message == MSG_XFERREADY)
    {
        g_xferReadySourceId.store(origin->Id);
        g_xferReadyAppId.store(destination->Id);
        g_sawXferReadyCallback.store(true);
        std::printf(
            "DSM DAT_NULL/MSG_XFERREADY: sourceId=%lu appId=%lu\n",
            static_cast<unsigned long>(origin->Id),
            static_cast<unsigned long>(destination->Id));
        return TWRC_SUCCESS;
    }

    return TWRC_FAILURE;
}

TW_HANDLE TW_CALLINGSTYLE SmokeDsmMemAllocate(TW_UINT32 size)
{
    return GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
}

void TW_CALLINGSTYLE SmokeDsmMemFree(TW_HANDLE handle)
{
    if (handle != nullptr)
    {
        GlobalFree(handle);
    }
}

TW_MEMREF TW_CALLINGSTYLE SmokeDsmMemLock(TW_HANDLE handle)
{
    return handle == nullptr ? nullptr : GlobalLock(handle);
}

void TW_CALLINGSTYLE SmokeDsmMemUnlock(TW_HANDLE handle)
{
    if (handle != nullptr)
    {
        GlobalUnlock(handle);
    }
}

bool WaitForXferReadyCallback(DWORD timeoutMilliseconds)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMilliseconds;
    while (!g_sawXferReadyCallback.load())
    {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
        {
            return false;
        }

        const auto remaining = static_cast<DWORD>((std::min)(deadline - now, static_cast<ULONGLONG>(25)));
        Sleep(remaining);
    }

    return true;
}

bool EnvironmentFlagEnabled(const wchar_t* name)
{
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0)
    {
        return false;
    }

    wchar_t value[16] = {};
    const auto valueLength = static_cast<DWORD>(sizeof(value) / sizeof(value[0]));
    const DWORD written = GetEnvironmentVariableW(name, value, valueLength);
    if (written == 0 || written >= valueLength)
    {
        return false;
    }

    return wcscmp(value, L"1") == 0 ||
        wcscmp(value, L"true") == 0 ||
        wcscmp(value, L"TRUE") == 0 ||
        wcscmp(value, L"yes") == 0 ||
        wcscmp(value, L"YES") == 0;
}

bool TryReadEnvironmentLong(const wchar_t* name, long& value)
{
    wchar_t buffer[32] = {};
    const auto bufferLength = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    const DWORD written = GetEnvironmentVariableW(name, buffer, bufferLength);
    if (written == 0 || written >= bufferLength)
    {
        return false;
    }

    wchar_t* end = nullptr;
    const long parsed = std::wcstol(buffer, &end, 10);
    if (end == buffer || end == nullptr || *end != L'\0')
    {
        return false;
    }

    value = parsed;
    return true;
}

void CopyTwainString(char* destination, size_t destinationSize, const char* source)
{
    std::memset(destination, 0, destinationSize);
    const size_t sourceLength = std::strlen(source);
    const size_t bytesToCopy = (std::min)(destinationSize - 1, sourceLength);
    std::memcpy(destination, source, bytesToCopy);
}

TW_UINT32 PackFix32(TW_FIX32 value)
{
    TW_UINT32 packed = 0;
    std::memcpy(&packed, &value, sizeof(packed));
    return packed;
}

TW_FIX32 MakeFix32(TW_INT16 whole)
{
    TW_FIX32 value{};
    value.Whole = whole;
    value.Frac = 0;
    return value;
}

size_t ItemSize(TW_UINT16 itemType)
{
    switch (itemType)
    {
    case TWTY_BOOL:
        return sizeof(TW_BOOL);
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

TW_HANDLE BuildOneValue(TW_UINT16 itemType, TW_UINT32 item)
{
    TW_HANDLE handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(TW_ONEVALUE));
    if (handle == nullptr)
    {
        return nullptr;
    }

    auto* oneValue = static_cast<pTW_ONEVALUE>(GlobalLock(handle));
    if (oneValue == nullptr)
    {
        GlobalFree(handle);
        return nullptr;
    }

    oneValue->ItemType = itemType;
    oneValue->Item = item;
    GlobalUnlock(handle);
    return handle;
}

void FreeContainer(TW_CAPABILITY& capability)
{
    if (capability.hContainer != nullptr)
    {
        GlobalFree(capability.hContainer);
        capability.hContainer = nullptr;
    }
}

bool ReadOneValue(TW_HANDLE handle, TW_UINT16 expectedItemType, TW_UINT32& item)
{
    auto* oneValue = static_cast<pTW_ONEVALUE>(GlobalLock(handle));
    if (oneValue == nullptr)
    {
        return false;
    }

    const bool ok = oneValue->ItemType == expectedItemType;
    if (ok)
    {
        item = oneValue->Item;
    }

    GlobalUnlock(handle);
    return ok;
}

bool EnumerationContains(TW_HANDLE handle, TW_UINT16 expectedItemType, TW_UINT32 packedNeedle)
{
    auto* enumeration = static_cast<pTW_ENUMERATION>(GlobalLock(handle));
    if (enumeration == nullptr)
    {
        return false;
    }

    const size_t itemSize = ItemSize(expectedItemType);
    bool found = false;
    if (enumeration->ItemType == expectedItemType && itemSize > 0)
    {
        for (TW_UINT32 index = 0; index < enumeration->NumItems; ++index)
        {
            TW_UINT32 current = 0;
            std::memcpy(&current, enumeration->ItemList + (index * itemSize), itemSize);
            if (current == packedNeedle)
            {
                found = true;
                break;
            }
        }
    }

    GlobalUnlock(handle);
    return found;
}

bool ArrayContains(TW_HANDLE handle, TW_UINT16 expectedItemType, TW_UINT32 packedNeedle)
{
    auto* array = static_cast<pTW_ARRAY>(GlobalLock(handle));
    if (array == nullptr)
    {
        return false;
    }

    const size_t itemSize = ItemSize(expectedItemType);
    bool found = false;
    if (array->ItemType == expectedItemType && itemSize > 0)
    {
        for (TW_UINT32 index = 0; index < array->NumItems; ++index)
        {
            TW_UINT32 current = 0;
            std::memcpy(&current, array->ItemList + (index * itemSize), itemSize);
            if (current == packedNeedle)
            {
                found = true;
                break;
            }
        }
    }

    GlobalUnlock(handle);
    return found;
}

TW_IDENTITY BuildAppIdentity()
{
    TW_IDENTITY identity{};
    identity.Id = 41;
    identity.ProtocolMajor = TWON_PROTOCOLMAJOR;
    identity.ProtocolMinor = TWON_PROTOCOLMINOR;
    identity.SupportedGroups = DG_CONTROL | DG_IMAGE | DF_APP2;
    identity.Version.MajorNum = 1;
    identity.Version.MinorNum = 0;
    identity.Version.Language = TWLG_ENGLISH_USA;
    identity.Version.Country = TWCY_USA;
    CopyTwainString(identity.Version.Info, sizeof(identity.Version.Info), "Smoke test");
    CopyTwainString(identity.Manufacturer, sizeof(identity.Manufacturer), "MBF");
    CopyTwainString(identity.ProductFamily, sizeof(identity.ProductFamily), "Test Host");
    CopyTwainString(identity.ProductName, sizeof(identity.ProductName), "SmokeDsEntry");
    return identity;
}

TW_ENTRYPOINT BuildEntryPoint()
{
    TW_ENTRYPOINT entryPoint{};
    entryPoint.Size = sizeof(entryPoint);
    entryPoint.DSM_Entry = SmokeDsmEntry;
    entryPoint.DSM_MemAllocate = SmokeDsmMemAllocate;
    entryPoint.DSM_MemFree = SmokeDsmMemFree;
    entryPoint.DSM_MemLock = SmokeDsmMemLock;
    entryPoint.DSM_MemUnlock = SmokeDsmMemUnlock;
    return entryPoint;
}

int ExpectSuccess(const char* label, TW_UINT16 returnCode)
{
    if (returnCode == TWRC_SUCCESS)
    {
        std::printf("%s: TWRC_SUCCESS\n", label);
        return 0;
    }

    std::printf("%s: unexpected return code %u\n", label, returnCode);
    return 1;
}

int ExpectFailure(const char* label, TW_UINT16 returnCode)
{
    if (returnCode == TWRC_FAILURE)
    {
        std::printf("%s: TWRC_FAILURE\n", label);
        return 0;
    }

    std::printf("%s: expected failure, got return code %u\n", label, returnCode);
    return 1;
}

int ExpectOneValue(
    const char* label,
    DsEntry entry,
    TW_IDENTITY& app,
    TW_UINT16 capabilityId,
    TW_UINT16 itemType,
    TW_UINT32 expectedItem)
{
    TW_CAPABILITY capability{};
    capability.Cap = capabilityId;
    int failures = ExpectSuccess(
        label,
        entry(&app, DG_CONTROL, DAT_CAPABILITY, MSG_GETCURRENT, &capability));
    if (failures == 0)
    {
        TW_UINT32 actual = 0;
        if (capability.ConType != TWON_ONEVALUE ||
            !ReadOneValue(capability.hContainer, itemType, actual) ||
            actual != expectedItem)
        {
            std::printf("%s: unexpected item %lu\n", label, static_cast<unsigned long>(actual));
            ++failures;
        }
    }

    FreeContainer(capability);
    return failures;
}

int SetOneValue(
    const char* label,
    DsEntry entry,
    TW_IDENTITY& app,
    TW_UINT16 capabilityId,
    TW_UINT16 itemType,
    TW_UINT32 item)
{
    TW_CAPABILITY capability{};
    capability.Cap = capabilityId;
    capability.ConType = TWON_ONEVALUE;
    capability.hContainer = BuildOneValue(itemType, item);
    if (capability.hContainer == nullptr)
    {
        std::printf("%s: failed to allocate input container\n", label);
        return 1;
    }

    const int failures = ExpectSuccess(
        label,
        entry(&app, DG_CONTROL, DAT_CAPABILITY, MSG_SET, &capability));
    FreeContainer(capability);
    return failures;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2)
    {
        fwprintf(stderr, L"usage: SmokeDsEntry.exe <path-to-ds-dll>\n");
        return 2;
    }

    HMODULE module = LoadLibraryW(argv[1]);
    if (module == nullptr)
    {
        fwprintf(stderr, L"LoadLibrary failed: %lu\n", GetLastError());
        return 2;
    }

    auto entry = reinterpret_cast<DsEntry>(GetProcAddress(module, "DS_Entry"));
    if (entry == nullptr)
    {
        fwprintf(stderr, L"GetProcAddress(DS_Entry) failed: %lu\n", GetLastError());
        FreeLibrary(module);
        return 2;
    }

    int failures = 0;
    TW_IDENTITY app = BuildAppIdentity();
    TW_IDENTITY source{};

    failures += ExpectSuccess(
        "DAT_IDENTITY/MSG_GET",
        entry(&app, DG_CONTROL, DAT_IDENTITY, MSG_GET, &source));
    std::printf("ProductName: %s\n", source.ProductName);
    source.Id = 17;

    TW_ENTRYPOINT entryPoint = BuildEntryPoint();
    failures += ExpectSuccess(
        "DAT_ENTRYPOINT/MSG_SET",
        entry(&app, DG_CONTROL, DAT_ENTRYPOINT, MSG_SET, &entryPoint));

    failures += ExpectSuccess(
        "DAT_IDENTITY/MSG_OPENDS",
        entry(&app, DG_CONTROL, DAT_IDENTITY, MSG_OPENDS, &source));
    const bool useMemoryTransfer = EnvironmentFlagEnabled(L"MBF_SMOKE_USE_MEMORY");

    TW_CAPABILITY supported{};
    supported.Cap = CAP_SUPPORTEDCAPS;
    failures += ExpectSuccess(
        "CAP_SUPPORTEDCAPS/MSG_GET",
        entry(&app, DG_CONTROL, DAT_CAPABILITY, MSG_GET, &supported));
    if (failures == 0 &&
        (supported.ConType != TWON_ENUMERATION ||
         !EnumerationContains(supported.hContainer, TWTY_UINT16, ICAP_PIXELTYPE) ||
         !EnumerationContains(supported.hContainer, TWTY_UINT16, CAP_XFERCOUNT) ||
         !EnumerationContains(supported.hContainer, TWTY_UINT16, CAP_DUPLEXENABLED) ||
         !EnumerationContains(supported.hContainer, TWTY_UINT16, ICAP_SUPPORTEDSIZES)))
    {
        std::printf("CAP_SUPPORTEDCAPS: missing required capabilities\n");
        ++failures;
    }
    FreeContainer(supported);

    TW_CAPABILITY supportedDats{};
    supportedDats.Cap = CAP_SUPPORTEDDATS;
    failures += ExpectSuccess(
        "CAP_SUPPORTEDDATS/MSG_GET",
        entry(&app, DG_CONTROL, DAT_CAPABILITY, MSG_GET, &supportedDats));
    if (failures == 0 &&
        (supportedDats.ConType != TWON_ARRAY ||
         !ArrayContains(supportedDats.hContainer, TWTY_UINT32, (DG_CONTROL << 16) | DAT_ENTRYPOINT) ||
         !ArrayContains(supportedDats.hContainer, TWTY_UINT32, (DG_CONTROL << 16) | DAT_EVENT) ||
         !ArrayContains(supportedDats.hContainer, TWTY_UINT32, (DG_CONTROL << 16) | DAT_PENDINGXFERS) ||
         !ArrayContains(supportedDats.hContainer, TWTY_UINT32, (DG_CONTROL << 16) | DAT_XFERGROUP) ||
         !ArrayContains(supportedDats.hContainer, TWTY_UINT32, (DG_IMAGE << 16) | DAT_IMAGENATIVEXFER)))
    {
        std::printf("CAP_SUPPORTEDDATS: missing required DATs\n");
        ++failures;
    }
    FreeContainer(supportedDats);

    failures += ExpectOneValue(
        "CAP_XFERCOUNT/default current",
        entry,
        app,
        CAP_XFERCOUNT,
        TWTY_INT16,
        0xffff);
    failures += SetOneValue("CAP_XFERCOUNT/SET 1", entry, app, CAP_XFERCOUNT, TWTY_INT16, 1);
    failures += ExpectOneValue(
        "CAP_XFERCOUNT/current 1",
        entry,
        app,
        CAP_XFERCOUNT,
        TWTY_INT16,
        1);
    TW_CAPABILITY xferCountReset{};
    xferCountReset.Cap = CAP_XFERCOUNT;
    failures += ExpectSuccess(
        "CAP_XFERCOUNT/RESET",
        entry(&app, DG_CONTROL, DAT_CAPABILITY, MSG_RESET, &xferCountReset));
    FreeContainer(xferCountReset);

    failures += ExpectOneValue(
        "ICAP_PIXELTYPE/default current",
        entry,
        app,
        ICAP_PIXELTYPE,
        TWTY_UINT16,
        TWPT_RGB);
    failures += SetOneValue("ICAP_PIXELTYPE/SET gray", entry, app, ICAP_PIXELTYPE, TWTY_UINT16, TWPT_GRAY);
    failures += ExpectOneValue(
        "ICAP_PIXELTYPE/current gray",
        entry,
        app,
        ICAP_PIXELTYPE,
        TWTY_UINT16,
        TWPT_GRAY);

    TW_CAPABILITY pixelReset{};
    pixelReset.Cap = ICAP_PIXELTYPE;
    failures += ExpectSuccess(
        "ICAP_PIXELTYPE/RESET",
        entry(&app, DG_CONTROL, DAT_CAPABILITY, MSG_RESET, &pixelReset));
    FreeContainer(pixelReset);
    failures += ExpectOneValue(
        "ICAP_PIXELTYPE/current reset",
        entry,
        app,
        ICAP_PIXELTYPE,
        TWTY_UINT16,
        TWPT_RGB);

    failures += SetOneValue("CAP_DUPLEXENABLED/SET true", entry, app, CAP_DUPLEXENABLED, TWTY_BOOL, TRUE);
    failures += ExpectOneValue(
        "CAP_DUPLEXENABLED/current true",
        entry,
        app,
        CAP_DUPLEXENABLED,
        TWTY_BOOL,
        TRUE);

    failures += ExpectOneValue(
        "ICAP_SUPPORTEDSIZES/default current",
        entry,
        app,
        ICAP_SUPPORTEDSIZES,
        TWTY_UINT16,
        TWSS_A4LETTER);

    TW_CAPABILITY supportedSizes{};
    supportedSizes.Cap = ICAP_SUPPORTEDSIZES;
    failures += ExpectSuccess(
        "ICAP_SUPPORTEDSIZES/MSG_GET",
        entry(&app, DG_CONTROL, DAT_CAPABILITY, MSG_GET, &supportedSizes));
    if (failures == 0 &&
        (supportedSizes.ConType != TWON_ENUMERATION ||
         !EnumerationContains(supportedSizes.hContainer, TWTY_UINT16, TWSS_A4LETTER) ||
         !EnumerationContains(supportedSizes.hContainer, TWTY_UINT16, TWSS_A3)))
    {
        std::printf("ICAP_SUPPORTEDSIZES/MSG_GET: missing A4 or A3\n");
        ++failures;
    }
    FreeContainer(supportedSizes);

    failures += SetOneValue("ICAP_SUPPORTEDSIZES/SET A3", entry, app, ICAP_SUPPORTEDSIZES, TWTY_UINT16, TWSS_A3);
    failures += ExpectOneValue(
        "ICAP_SUPPORTEDSIZES/current A3",
        entry,
        app,
        ICAP_SUPPORTEDSIZES,
        TWTY_UINT16,
        TWSS_A3);

    failures += SetOneValue(
        "ICAP_XRESOLUTION/SET 600",
        entry,
        app,
        ICAP_XRESOLUTION,
        TWTY_FIX32,
        PackFix32(MakeFix32(600)));
    failures += ExpectOneValue(
        "ICAP_XRESOLUTION/current 600",
        entry,
        app,
        ICAP_XRESOLUTION,
        TWTY_FIX32,
        PackFix32(MakeFix32(600)));

    if (useMemoryTransfer)
    {
        failures += SetOneValue(
            "ICAP_XFERMECH/SET memory",
            entry,
            app,
            ICAP_XFERMECH,
            TWTY_UINT16,
            TWSX_MEMORY);
        failures += ExpectOneValue(
            "ICAP_XFERMECH/current memory",
            entry,
            app,
            ICAP_XFERMECH,
            TWTY_UINT16,
            TWSX_MEMORY);
    }
    else
    {
        failures += ExpectOneValue(
            "ICAP_XFERMECH/current native",
            entry,
            app,
            ICAP_XFERMECH,
            TWTY_UINT16,
            TWSX_NATIVE);
    }

    TW_CAPABILITY querySupport{};
    querySupport.Cap = ICAP_XFERMECH;
    failures += ExpectSuccess(
        "ICAP_XFERMECH/QUERYSUPPORT",
        entry(&app, DG_CONTROL, DAT_CAPABILITY, MSG_QUERYSUPPORT, &querySupport));
    if (failures == 0)
    {
        TW_UINT32 support = 0;
        if (querySupport.ConType != TWON_ONEVALUE ||
            !ReadOneValue(querySupport.hContainer, TWTY_INT32, support) ||
            (support & (TWQC_GET | TWQC_SET | TWQC_RESET)) != (TWQC_GET | TWQC_SET | TWQC_RESET))
        {
            std::printf("ICAP_XFERMECH/QUERYSUPPORT: unexpected support mask %lu\n", static_cast<unsigned long>(support));
            ++failures;
        }
    }
    FreeContainer(querySupport);

    TW_CAPABILITY invalidPixel{};
    invalidPixel.Cap = ICAP_PIXELTYPE;
    invalidPixel.ConType = TWON_ONEVALUE;
    invalidPixel.hContainer = BuildOneValue(TWTY_UINT16, TWPT_PALETTE);
    failures += ExpectFailure(
        "ICAP_PIXELTYPE/SET invalid",
        entry(&app, DG_CONTROL, DAT_CAPABILITY, MSG_SET, &invalidPixel));
    FreeContainer(invalidPixel);

    TW_CAPABILITY invalidPaper{};
    invalidPaper.Cap = ICAP_SUPPORTEDSIZES;
    invalidPaper.ConType = TWON_ONEVALUE;
    invalidPaper.hContainer = BuildOneValue(TWTY_UINT16, TWSS_USLETTER);
    failures += ExpectFailure(
        "ICAP_SUPPORTEDSIZES/SET invalid",
        entry(&app, DG_CONTROL, DAT_CAPABILITY, MSG_SET, &invalidPaper));
    FreeContainer(invalidPaper);

    TW_STATUS invalidStatus{};
    failures += ExpectSuccess(
        "DAT_STATUS/MSG_GET after invalid set",
        entry(&app, DG_CONTROL, DAT_STATUS, MSG_GET, &invalidStatus));
    if (invalidStatus.ConditionCode != TWCC_BADVALUE)
    {
        std::printf(
            "DAT_STATUS after invalid set: expected %u, got %u\n",
            TWCC_BADVALUE,
            invalidStatus.ConditionCode);
        ++failures;
    }

    long configuredXferCount = 0;
    if (TryReadEnvironmentLong(L"MBF_SMOKE_SET_XFERCOUNT", configuredXferCount))
    {
        if (configuredXferCount == 0 || configuredXferCount < -1 || configuredXferCount > 0x7fff)
        {
            std::printf(
                "MBF_SMOKE_SET_XFERCOUNT: invalid value %ld\n",
                configuredXferCount);
            ++failures;
        }
        else
        {
            const TW_UINT32 packedXferCount =
                static_cast<TW_UINT16>(static_cast<TW_INT16>(configuredXferCount));
            failures += SetOneValue(
                "CAP_XFERCOUNT/SET configured",
                entry,
                app,
                CAP_XFERCOUNT,
                TWTY_INT16,
                packedXferCount);
            failures += ExpectOneValue(
                "CAP_XFERCOUNT/current configured",
                entry,
                app,
                CAP_XFERCOUNT,
                TWTY_INT16,
                packedXferCount);
        }
    }

    TW_UINT16 expectedTransferTotal = 1;
    long expectedTransferTotalValue = 0;
    if (TryReadEnvironmentLong(L"MBF_SMOKE_EXPECT_TRANSFER_TOTAL", expectedTransferTotalValue))
    {
        if (expectedTransferTotalValue <= 0 || expectedTransferTotalValue > 0xffff)
        {
            std::printf(
                "MBF_SMOKE_EXPECT_TRANSFER_TOTAL: invalid value %ld\n",
                expectedTransferTotalValue);
            ++failures;
        }
        else
        {
            expectedTransferTotal = static_cast<TW_UINT16>(expectedTransferTotalValue);
        }
    }

    TW_USERINTERFACE userInterface{};
    const bool expectXferReady = EnvironmentFlagEnabled(L"MBF_SMOKE_EXPECT_XFERREADY");
    const bool expectEnableCallback = EnvironmentFlagEnabled(L"MBF_SMOKE_EXPECT_ENABLE_CALLBACK");
    const bool expectPaperA3 = EnvironmentFlagEnabled(L"MBF_SMOKE_EXPECT_PAPER_A3");
    userInterface.ShowUI = expectXferReady ? TRUE : FALSE;
    userInterface.ModalUI = FALSE;
    failures += ExpectSuccess(
        "DAT_USERINTERFACE/MSG_ENABLEDS",
        entry(&app, DG_CONTROL, DAT_USERINTERFACE, MSG_ENABLEDS, &userInterface));

    if (expectEnableCallback)
    {
        if (WaitForXferReadyCallback(2500))
        {
            std::printf("DSM DAT_NULL/MSG_XFERREADY: observed before first DAT_EVENT\n");
        }
        else
        {
            std::printf("DSM DAT_NULL/MSG_XFERREADY: timed out before first DAT_EVENT\n");
            ++failures;
        }
    }

    TW_EVENT event{};
    const TW_UINT16 eventReturn = entry(&app, DG_CONTROL, DAT_EVENT, MSG_PROCESSEVENT, &event);
    if (expectXferReady)
    {
        if (eventReturn == TWRC_DSEVENT && event.TWMessage == MSG_XFERREADY)
        {
            std::printf("DAT_EVENT/MSG_PROCESSEVENT: MSG_XFERREADY\n");
            if (!g_sawXferReadyCallback.load())
            {
                std::printf("DSM DAT_NULL/MSG_XFERREADY: callback was not observed\n");
                ++failures;
            }
            if (g_xferReadySourceId.load() != 17 || g_xferReadyAppId.load() != 41)
            {
                std::printf(
                    "DSM DAT_NULL/MSG_XFERREADY: unexpected ids source=%lu app=%lu\n",
                    static_cast<unsigned long>(g_xferReadySourceId.load()),
                    static_cast<unsigned long>(g_xferReadyAppId.load()));
                ++failures;
            }

            if (expectPaperA3)
            {
                failures += ExpectOneValue(
                    "ICAP_SUPPORTEDSIZES/current A3 from IPC",
                    entry,
                    app,
                    ICAP_SUPPORTEDSIZES,
                    TWTY_UINT16,
                    TWSS_A3);
            }

            TW_UINT32 transferGroup = 0;
            failures += ExpectSuccess(
                "DAT_XFERGROUP/MSG_GET",
                entry(&app, DG_CONTROL, DAT_XFERGROUP, MSG_GET, &transferGroup));
            if (transferGroup != DG_IMAGE)
            {
                std::printf(
                    "DAT_XFERGROUP/MSG_GET: expected DG_IMAGE, got %lu\n",
                    static_cast<unsigned long>(transferGroup));
                ++failures;
            }

            TW_PENDINGXFERS readyPendingTransfers{};
            failures += ExpectSuccess(
                "DAT_PENDINGXFERS/MSG_GET",
                entry(&app, DG_CONTROL, DAT_PENDINGXFERS, MSG_GET, &readyPendingTransfers));
            if (readyPendingTransfers.Count != expectedTransferTotal)
            {
                std::printf(
                    "DAT_PENDINGXFERS/MSG_GET: expected %u remaining, got %u\n",
                    expectedTransferTotal,
                    readyPendingTransfers.Count);
                ++failures;
            }

            if (useMemoryTransfer)
            {
                if (expectedTransferTotal != 1)
                {
                    std::printf(
                        "DAT_IMAGEMEMXFER smoke: expectedTransferTotal=%u is unsupported in this test path\n",
                        expectedTransferTotal);
                    ++failures;
                }
                else
                {
                    TW_SETUPMEMXFER setup{};
                    failures += ExpectSuccess(
                        "DAT_SETUPMEMXFER/MSG_GET",
                        entry(&app, DG_CONTROL, DAT_SETUPMEMXFER, MSG_GET, &setup));
                    if (setup.MinBufSize == 0 || setup.Preferred < setup.MinBufSize || setup.MaxBufSize < setup.Preferred)
                    {
                        std::printf("DAT_SETUPMEMXFER/MSG_GET: invalid buffer sizes\n");
                        ++failures;
                    }

                    BYTE buffer[6] = {};
                    TW_IMAGEMEMXFER memoryTransfer{};
                    memoryTransfer.Memory.Flags = TWMF_APPOWNS | TWMF_POINTER;
                    memoryTransfer.Memory.Length = sizeof(buffer);
                    memoryTransfer.Memory.TheMem = buffer;

                    TW_UINT16 transferReturn =
                        entry(&app, DG_IMAGE, DAT_IMAGEMEMXFER, MSG_GET, &memoryTransfer);
                    if (transferReturn != TWRC_SUCCESS ||
                        memoryTransfer.BytesWritten != sizeof(buffer) ||
                        memoryTransfer.Rows != 1 ||
                        memoryTransfer.YOffset != 0)
                    {
                        std::printf(
                            "DAT_IMAGEMEMXFER first strip: unexpected rc=%u bytes=%lu rows=%lu y=%lu\n",
                            transferReturn,
                            static_cast<unsigned long>(memoryTransfer.BytesWritten),
                            static_cast<unsigned long>(memoryTransfer.Rows),
                            static_cast<unsigned long>(memoryTransfer.YOffset));
                        ++failures;
                    }
                    else
                    {
                        std::printf("DAT_IMAGEMEMXFER/MSG_GET: first strip OK\n");
                    }

                    std::memset(buffer, 0, sizeof(buffer));
                    memoryTransfer = {};
                    memoryTransfer.Memory.Flags = TWMF_APPOWNS | TWMF_POINTER;
                    memoryTransfer.Memory.Length = sizeof(buffer);
                    memoryTransfer.Memory.TheMem = buffer;

                    transferReturn = entry(&app, DG_IMAGE, DAT_IMAGEMEMXFER, MSG_GET, &memoryTransfer);
                    if (transferReturn != TWRC_XFERDONE ||
                        memoryTransfer.BytesWritten != sizeof(buffer) ||
                        memoryTransfer.Rows != 1 ||
                        memoryTransfer.YOffset != 1)
                    {
                        std::printf(
                            "DAT_IMAGEMEMXFER final strip: unexpected rc=%u bytes=%lu rows=%lu y=%lu\n",
                            transferReturn,
                            static_cast<unsigned long>(memoryTransfer.BytesWritten),
                            static_cast<unsigned long>(memoryTransfer.Rows),
                            static_cast<unsigned long>(memoryTransfer.YOffset));
                        ++failures;
                    }
                    else
                    {
                        std::printf("DAT_IMAGEMEMXFER/MSG_GET: final strip XFERDONE\n");
                    }

                    TW_PENDINGXFERS pendingTransfers{};
                    failures += ExpectSuccess(
                        "DAT_PENDINGXFERS/MSG_ENDXFER",
                        entry(&app, DG_CONTROL, DAT_PENDINGXFERS, MSG_ENDXFER, &pendingTransfers));
                    if (pendingTransfers.Count != 0)
                    {
                        std::printf(
                            "DAT_PENDINGXFERS/MSG_ENDXFER: expected 0 remaining, got %u\n",
                            pendingTransfers.Count);
                        ++failures;
                    }
                }
            }
            else
            {
                for (TW_UINT16 transferIndex = 0; transferIndex < expectedTransferTotal; ++transferIndex)
                {
                    char imageInfoLabel[64] = {};
                    std::snprintf(
                        imageInfoLabel,
                        sizeof(imageInfoLabel),
                        "DAT_IMAGEINFO/MSG_GET #%u",
                        static_cast<unsigned>(transferIndex + 1));

                    TW_IMAGEINFO imageInfo{};
                    failures += ExpectSuccess(
                        imageInfoLabel,
                        entry(&app, DG_IMAGE, DAT_IMAGEINFO, MSG_GET, &imageInfo));
                    if (imageInfo.ImageWidth <= 0 || imageInfo.ImageLength <= 0)
                    {
                        std::printf(
                            "%s: invalid image size %ld x %ld\n",
                            imageInfoLabel,
                            static_cast<long>(imageInfo.ImageWidth),
                            static_cast<long>(imageInfo.ImageLength));
                        ++failures;
                    }

                    TW_HANDLE dib = nullptr;
                    const TW_UINT16 transferReturn =
                        entry(&app, DG_IMAGE, DAT_IMAGENATIVEXFER, MSG_GET, &dib);
                    LONG transferredWidth = 0;
                    LONG transferredHeight = 0;
                    TW_UINT16 transferredBitsPerPixel = 0;
                    if (transferReturn == TWRC_XFERDONE && dib != nullptr)
                    {
                        auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(dib));
                        if (header == nullptr ||
                            header->biSize != sizeof(BITMAPINFOHEADER) ||
                            header->biWidth <= 0 ||
                            header->biHeight <= 0 ||
                            header->biSizeImage == 0)
                        {
                            std::printf(
                                "DAT_IMAGENATIVEXFER/MSG_GET #%u: invalid DIB\n",
                                static_cast<unsigned>(transferIndex + 1));
                            ++failures;
                        }
                        else
                        {
                            transferredWidth = header->biWidth;
                            transferredHeight = header->biHeight;
                            transferredBitsPerPixel = header->biBitCount;
                            std::printf(
                                "DAT_IMAGENATIVEXFER/MSG_GET #%u: XFERDONE %ld x %ld %u bpp\n",
                                static_cast<unsigned>(transferIndex + 1),
                                static_cast<long>(header->biWidth),
                                static_cast<long>(header->biHeight),
                                header->biBitCount);
                        }

                        if (header != nullptr)
                        {
                            GlobalUnlock(dib);
                        }
                        GlobalFree(dib);

                        char postTransferLabel[80] = {};
                        std::snprintf(
                            postTransferLabel,
                            sizeof(postTransferLabel),
                            "DAT_IMAGEINFO/MSG_GET after native xfer #%u",
                            static_cast<unsigned>(transferIndex + 1));

                        TW_IMAGEINFO postTransferInfo{};
                        failures += ExpectSuccess(
                            postTransferLabel,
                            entry(&app, DG_IMAGE, DAT_IMAGEINFO, MSG_GET, &postTransferInfo));
                        if (postTransferInfo.ImageWidth != transferredWidth ||
                            postTransferInfo.ImageLength != transferredHeight ||
                            postTransferInfo.BitsPerPixel != transferredBitsPerPixel)
                        {
                            std::printf(
                                "%s: expected %ld x %ld %u bpp, got %ld x %ld %d bpp\n",
                                postTransferLabel,
                                static_cast<long>(transferredWidth),
                                static_cast<long>(transferredHeight),
                                transferredBitsPerPixel,
                                static_cast<long>(postTransferInfo.ImageWidth),
                                static_cast<long>(postTransferInfo.ImageLength),
                                postTransferInfo.BitsPerPixel);
                            ++failures;
                        }
                    }
                    else
                    {
                        std::printf(
                            "DAT_IMAGENATIVEXFER/MSG_GET #%u: expected XFERDONE, got rc=%u handle=%p\n",
                            static_cast<unsigned>(transferIndex + 1),
                            transferReturn,
                            dib);
                        ++failures;
                    }

                    char endXferLabel[72] = {};
                    std::snprintf(
                        endXferLabel,
                        sizeof(endXferLabel),
                        "DAT_PENDINGXFERS/MSG_ENDXFER #%u",
                        static_cast<unsigned>(transferIndex + 1));

                    TW_PENDINGXFERS pendingTransfers{};
                    failures += ExpectSuccess(
                        endXferLabel,
                        entry(&app, DG_CONTROL, DAT_PENDINGXFERS, MSG_ENDXFER, &pendingTransfers));
                    const TW_UINT16 expectedRemaining =
                        static_cast<TW_UINT16>(expectedTransferTotal - (transferIndex + 1));
                    if (pendingTransfers.Count != expectedRemaining)
                    {
                        std::printf(
                            "%s: expected %u remaining, got %u\n",
                            endXferLabel,
                            expectedRemaining,
                            pendingTransfers.Count);
                        ++failures;
                    }
                }
            }
        }
        else
        {
            std::printf(
                "DAT_EVENT/MSG_PROCESSEVENT: expected XFERREADY, got rc=%u message=%u\n",
                eventReturn,
                event.TWMessage);
            ++failures;
        }
    }
    else if (eventReturn == TWRC_NOTDSEVENT)
    {
        std::printf("DAT_EVENT/MSG_PROCESSEVENT: TWRC_NOTDSEVENT\n");
    }
    else
    {
        std::printf(
            "DAT_EVENT/MSG_PROCESSEVENT: expected NOTDSEVENT, got rc=%u message=%u\n",
            eventReturn,
            event.TWMessage);
        ++failures;
    }

    failures += ExpectSuccess(
        "DAT_USERINTERFACE/MSG_DISABLEDS",
        entry(&app, DG_CONTROL, DAT_USERINTERFACE, MSG_DISABLEDS, &userInterface));

    failures += ExpectSuccess(
        "DAT_IDENTITY/MSG_CLOSEDS",
        entry(&app, DG_CONTROL, DAT_IDENTITY, MSG_CLOSEDS, &source));

    TW_STATUS status{};
    failures += ExpectSuccess(
        "DAT_STATUS/MSG_GET",
        entry(&app, DG_CONTROL, DAT_STATUS, MSG_GET, &status));
    std::printf("ConditionCode: %u\n", status.ConditionCode);

    FreeLibrary(module);
    return failures == 0 ? 0 : 1;
}
