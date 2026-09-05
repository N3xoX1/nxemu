#pragma once
#include <nxemu-core/notification.h>

class AndroidNotification :
    public INotification
{
public:
    AndroidNotification();

    // INotification
    void DisplayError(const char * message, const char * title) const override;
    NotificationResponse Query(const char * message, const char * title) const override;
    void BreakPoint(const char * fileName, uint32_t lineNumber) override;
    void AppInitDone() override;

    static AndroidNotification & GetInstance();

private:
    AndroidNotification(const AndroidNotification &) = delete;
    AndroidNotification & operator=(const AndroidNotification &) = delete;
};
