#pragma once
#include "dialog.h"

class AboutDialog final : public Dialog
{
public:
    AboutDialog(Rml::Context * context, const std::filesystem::path & assetDir);

private:
    void OnOpen() override;
};
