#include "ui_activity.h"
#include "ui_host.h"
#include "ui_screen.h"
#include "ui_thread.h"

#include <android/asset_manager_jni.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <map>
#include <memory>
#include <yuzu_common/android/java_bridge.h>

namespace
{
    std::atomic<jlong> s_nextScreen = 1;
    std::map<jlong, std::unique_ptr<UiScreen>> s_screens;

    UiScreen * FindScreen(jlong handle)
    {
        const auto itr = s_screens.find(handle);
        return itr != s_screens.end() ? itr->second.get() : nullptr;
    }

    void WithScreen(jlong handle, std::function<void(UiScreen &)> work)
    {
        UiThread::Instance().Post([handle, work = std::move(work)] {
            if (UiScreen * screen = FindScreen(handle))
            {
                work(*screen);
            }
        });
    }
}

extern "C" JNIEXPORT void JNICALL Java_org_nxemu_ui_main_NativeUi_start(JNIEnv * env, jobject /*thiz*/, jobject assetManager)
{
    UiThread::Instance().Start(AAssetManager_fromJava(env, env->NewGlobalRef(assetManager)));
}

extern "C" JNIEXPORT jlong JNICALL Java_org_nxemu_ui_main_NativeUi_create(JNIEnv * env, jobject /*thiz*/, jobject host, jint mode)
{
    const jlong handle = s_nextScreen++;
    auto screen = std::make_shared<std::unique_ptr<UiScreen>>(
        std::make_unique<UiScreen>(std::make_unique<UiHost>(env, host), static_cast<UiActivity::Mode>(mode)));
    UiThread::Instance().Post([handle, screen] {
        if ((*screen)->Create())
        {
            s_screens[handle] = std::move(*screen);
        }
    });
    return handle;
}

extern "C" JNIEXPORT void JNICALL Java_org_nxemu_ui_main_NativeUi_destroy(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle)
{
    UiThread::Instance().Run([handle] { s_screens.erase(handle); });
}

extern "C" JNIEXPORT void JNICALL Java_org_nxemu_ui_main_NativeUi_setSurface(JNIEnv * env, jobject /*thiz*/, jlong handle, jobject surface,
                                                                            jint width, jint height)
{
    if (surface == nullptr)
    {
        UiThread::Instance().Run([handle] {
            if (UiScreen * screen = FindScreen(handle))
            {
                screen->SetWindow(nullptr, 0, 0);
            }
        });
        return;
    }
    ANativeWindow * window = ANativeWindow_fromSurface(env, surface);
    UiThread::Instance().Post([handle, window, width, height] {
        if (UiScreen * screen = FindScreen(handle))
        {
            screen->SetWindow(window, width, height);
        }
        else if (window != nullptr)
        {
            ANativeWindow_release(window);
        }
    });
}

extern "C" JNIEXPORT void JNICALL Java_org_nxemu_ui_main_NativeUi_setMetrics(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle, jfloat density,
                                                                            jint left, jint top, jint right, jint bottom)
{
    WithScreen(handle, [=](UiScreen & screen) { screen.SetMetrics(density, left, top, right, bottom); });
}

extern "C" JNIEXPORT void JNICALL Java_org_nxemu_ui_main_NativeUi_touch(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle, jint action, jfloat x,
                                                                       jfloat y)
{
    WithScreen(handle, [=](UiScreen & screen) { screen.Touch(action, x, y); });
}

extern "C" JNIEXPORT void JNICALL Java_org_nxemu_ui_main_NativeUi_event(JNIEnv * env, jobject /*thiz*/, jlong handle, jstring name, jstring payload)
{
    WithScreen(handle, [name = GetJString(env, name), payload = GetJString(env, payload)](UiScreen & screen) { screen.Event(name, payload); });
}
