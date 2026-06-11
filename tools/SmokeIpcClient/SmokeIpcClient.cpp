#include <cstdio>

#include "ScannerIpcClient.h"

int wmain()
{
    mbf::twain::ScannerIpcClient client;
    if (!client.Ping(3000))
    {
        std::printf("PING failed\n");
        return 1;
    }

    mbf::twain::ScannerIpcState state{};
    if (!client.TryGetState(state, 3000))
    {
        std::printf("GET_STATE failed\n");
        return 1;
    }

    std::printf(
        "revision=%lu duplex=%u pixel=%ls xres=%lu yres=%lu scan=%u images=%zu\n",
        static_cast<unsigned long>(state.revision),
        state.duplexEnabled ? 1U : 0U,
        state.pixelType.c_str(),
        static_cast<unsigned long>(state.xResolution),
        static_cast<unsigned long>(state.yResolution),
        state.scanRequested ? 1U : 0U,
        state.selectedImages.size());
    return 0;
}
