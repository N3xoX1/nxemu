#include "system_theme.h"
#if defined(_WIN32)
#include <windows.h>
#elif !defined(__APPLE__)
#include <dbus/dbus.h>
#endif

#if defined(_WIN32)
bool SystemPrefersDarkTheme()
{
    DWORD light = 0;
    DWORD size = sizeof(light);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return status != ERROR_SUCCESS || light == 0;
}
#elif !defined(__APPLE__)
bool SystemPrefersDarkTheme()
{
    if (!dbus_threads_init_default())
    {
        return true;
    }
    DBusConnection * connection = dbus_bus_get_private(DBUS_BUS_SESSION, nullptr);
    if (connection == nullptr)
    {
        return true;
    }
    dbus_connection_set_exit_on_disconnect(connection, false);
    DBusMessage * request = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Settings", "Read");
    DBusMessage * reply = nullptr;
    if (request != nullptr)
    {
        const char * space = "org.freedesktop.appearance";
        const char * key = "color-scheme";
        dbus_message_append_args(request, DBUS_TYPE_STRING, &space, DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID);
        reply = dbus_connection_send_with_reply_and_block(connection, request, 250, nullptr);
        dbus_message_unref(request);
    }
    dbus_uint32_t scheme = 0;
    DBusMessageIter value;
    if (reply != nullptr && dbus_message_iter_init(reply, &value))
    {
        while (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_VARIANT)
        {
            DBusMessageIter inner;
            dbus_message_iter_recurse(&value, &inner);
            value = inner;
        }
        if (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_UINT32)
        {
            dbus_message_iter_get_basic(&value, &scheme);
        }
    }
    if (reply != nullptr)
    {
        dbus_message_unref(reply);
    }
    dbus_connection_close(connection);
    dbus_connection_unref(connection);
    return scheme != 2;
}
#endif
