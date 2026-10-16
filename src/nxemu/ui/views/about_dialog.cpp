#include "about_dialog.h"
#include "rml_events.h"
#include <RmlUi/Core/ElementDocument.h>
#include <nxemu-core/version.h>
#include <nxemu/ui/host/shell.h>

AboutDialog::AboutDialog(Rml::Context * context, const std::filesystem::path & assetDir) :
    Dialog(context, assetDir / "about_dialog.rml")
{
}

void AboutDialog::OnOpen()
{
    SetText("about-version", VER_FILE_VERSION_STR);
    SetText("about-copyright", VER_COPYRIGHT_STR);
    UiEvents::Listen(Document(), "click", [](Rml::Event & event) {
        Rml::Element * target = event.GetTargetElement();
        if (target->HasAttribute("data-url"))
        {
            OpenUrl(target->GetAttribute<Rml::String>("data-url", ""));
        }
    });
}
