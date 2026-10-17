#pragma once
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <functional>

namespace UiEvents
{
void Listen(Rml::Element * element, const char * type, std::function<void(Rml::Event &)> callback);

void ListenFirst(Rml::Element * element, const char * type, std::function<void(Rml::Event &)> callback);

Rml::Element * ClosestWithClass(Rml::Element * element, const char * className);
} // namespace UiEvents
