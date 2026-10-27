#pragma once
struct IVideo
{
    virtual ~IVideo() = default;
    virtual bool WaitForGuestGpu() = 0;
};
struct StubSystemModules
{
    IVideo *video = nullptr;
    IVideo &Video()
    {
        return *video;
    }
};
