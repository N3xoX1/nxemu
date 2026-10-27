#pragma once

#include <string>

struct AAssetManager;

namespace UiLanguage
{
void Start(AAssetManager * assets, const std::string & deviceLocales);
void Apply();

std::string Choice();
void SetChoice(const std::string & code);
std::string Code();
int Generation();
} // namespace UiLanguage
