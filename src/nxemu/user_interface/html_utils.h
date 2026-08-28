#pragma once

#include <sciter_handler.h>
#include <sciter_ui.h>
#include <string>

class SciterElement;

std::string HtmlEscape(const std::string & text);
std::string ImageDataUri(const uint8_t * data, size_t size);
std::string ImageDataUriFromFile(const char * path);
void AttachClickHandler(ISciterUI & sciterUI, const SciterElement & element, IClickSink * sink);
void SetElementEnabled(const SciterElement & root, const char * id, bool enabled);
void SetElementVisible(const SciterElement & root, const char * id, bool visible);
void SetInputValue(const SciterElement & root, const char * id, const std::string & value);
