#include "notification.h"

INotification * g_notify;

void SetNotification(INotification * notification)
{
    g_notify = notification;
}
