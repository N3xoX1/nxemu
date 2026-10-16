#pragma once
#include <nxemu-module-spec/base.h>
#include <stdint.h>

nxinterface INotification
{
    //Error Messages
    virtual void DisplayError(const char * message, const char * title) const = 0;
    virtual NotificationResponse Query(const char * message, const char * title) const = 0;
    virtual void BreakPoint(const char * fileName, uint32_t lineNumber) = 0;
};

extern INotification * g_notify;
