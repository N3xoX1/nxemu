#pragma once
#include <memory>
#include <nxemu-core/notification.h>
#include <sciter_ui.h>

class Notification :
    public INotification
{
public:
    Notification();

    // INotification
    void DisplayError(const char * message, const char * title) const override;
    NotificationResponse Query(const char * message, const char * title) const override;
    void BreakPoint(const char * fileName, uint32_t lineNumber) override;
    void AppInitDone(void) override;

    void SetSciterContext(ISciterUI * sciterUI, void * parentWindow);
    void ClearSciterContext();

    static Notification & GetInstance();
    static void CleanUp();

private:
    Notification(const Notification &) = delete;
    Notification & operator=(const Notification &) = delete;

    static std::unique_ptr<Notification> s_instance;

    ISciterUI * m_sciterUI;
    void * m_parentWindow;
};