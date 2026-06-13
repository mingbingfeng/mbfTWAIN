#pragma once

namespace mbf::twain
{

class IpcSession final
{
public:
    bool AwaitingUiSelection() const noexcept { return awaitingUiSelection_; }
    bool CloseDsRequested() const noexcept { return closeDsRequest_; }
    bool CloseDsRequestNotified() const noexcept { return closeDsRequestNotified_; }

    void BeginAwaitingUiSelection() noexcept
    {
        awaitingUiSelection_ = true;
        closeDsRequest_ = false;
        closeDsRequestNotified_ = false;
    }

    void ClearAwaitingUiSelection() noexcept
    {
        awaitingUiSelection_ = false;
    }

    void RequestCloseDs() noexcept
    {
        awaitingUiSelection_ = false;
        closeDsRequest_ = true;
    }

    void MarkCloseDsRequestNotified() noexcept
    {
        closeDsRequestNotified_ = true;
    }

    void ResetUiFlow() noexcept
    {
        awaitingUiSelection_ = false;
        closeDsRequest_ = false;
        closeDsRequestNotified_ = false;
    }

private:
    bool awaitingUiSelection_ = false;
    bool closeDsRequest_ = false;
    bool closeDsRequestNotified_ = false;
};

} // namespace mbf::twain
