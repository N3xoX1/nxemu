#pragma once
#include <nxemu-module-spec/base.h>

nxinterface INotification;

void SetNotification(INotification * notification);
bool AppInit(const char * baseDirectory, const char * appDirectory);
void AppCleanup();
