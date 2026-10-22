#pragma once
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <functional>

namespace UiEvents
{
// Calls the callback for events of the given type that reach the element,
// including those bubbling up from its descendants. The element owns the
// subscription. Attach to elements that outlive their own event handling and
// find the affected child with Closest(), rather than one listener per child.
void Listen(Rml::Element * element, const char * type, std::function<void(Rml::Event &)> callback);

// As Listen(), but called on the way down to the event's target: before the
// target and whatever is inside the element have handled the event.
void ListenFirst(Rml::Element * element, const char * type, std::function<void(Rml::Event &)> callback);

// The element itself or its nearest ancestor with the class, or nullptr.
Rml::Element * ClosestWithClass(Rml::Element * element, const char * className);
} // namespace UiEvents
