#include "rml_events.h"
#include <RmlUi/Core/EventListener.h>

namespace
{
class CallbackListener final : public Rml::EventListener
{
public:
    explicit CallbackListener(std::function<void(Rml::Event &)> callback) :
        m_callback(std::move(callback))
    {
    }

    void ProcessEvent(Rml::Event & event) override
    {
        m_callback(event);
    }

    void OnDetach(Rml::Element * /*element*/) override
    {
        delete this;
    }

private:
    std::function<void(Rml::Event &)> m_callback;
};
} // namespace

void UiEvents::Listen(Rml::Element * element, const char * type, std::function<void(Rml::Event &)> callback)
{
    if (element != nullptr)
    {
        element->AddEventListener(type, new CallbackListener(std::move(callback)));
    }
}

void UiEvents::ListenFirst(Rml::Element * element, const char * type, std::function<void(Rml::Event &)> callback)
{
    if (element != nullptr)
    {
        element->AddEventListener(type, new CallbackListener(std::move(callback)), true);
    }
}

Rml::Element * UiEvents::ClosestWithClass(Rml::Element * element, const char * className)
{
    while (element != nullptr && !element->IsClassSet(className))
    {
        element = element->GetParentNode();
    }
    return element;
}
