#pragma once
#include <nxemu-module-spec/base.h>
#include <sciter_ui.h>

nxinterface ISystemModules;
class SciterMainWindow;
class SystemModules;

static const char * IID_ROMBROWSER = "F68DFC0D-C86D-4810-97C6-48289FA650ED";

suinterface IRomBrowser
{
    void PopulateAsync() = 0;
    void SetMainWindow(SciterMainWindow * window, ISystemModules * modules) = 0;
    void ClearItems() = 0;
};

bool Register_WidgetRomBrowser(ISciterUI & SciterUI);