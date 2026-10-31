#pragma once
#include "dialog.h"

class AboutDialog final : public Dialog
{
public:
    AboutDialog(UiWindow & window, const std::filesystem::path & assetDir);

private:
    void OnOpen() override;
};
