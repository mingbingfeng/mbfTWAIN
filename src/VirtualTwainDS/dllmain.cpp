#include <windows.h>

#include <vector>

#include "DiagnosticsLog.h"

namespace
{

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

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        mbf::twain::diagnostics::AppendLine(
            L"DS-DllMain",
            L"PROCESS_ATTACH module=" + ModulePath(module));
        DisableThreadLibraryCalls(module);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        mbf::twain::diagnostics::AppendLine(
            L"DS-DllMain",
            L"PROCESS_DETACH module=" + ModulePath(module));
    }

    return TRUE;
}
