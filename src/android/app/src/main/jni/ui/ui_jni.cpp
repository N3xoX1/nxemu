#include "ui_activity.h"
#include "ui_host.h"
#include "ui_thread.h"
#include "ui_screen.h"

#include <android/asset_manager_jni.h>
#include <jni.h>
#include <map>
#include <memory>

namespace
{
    std::atomic<jlong> s_nextScreen = 1;
    std::map<jlong, std::unique_ptr<UiScreen>> s_screens;
}

extern "C" JNIEXPORT void JNICALL Java_org_nxemu_ui_main_NativeUi_start(JNIEnv * env, jobject /*thiz*/, jobject assetManager)
{
    UiThread::Instance().Start(AAssetManager_fromJava(env, env->NewGlobalRef(assetManager)));
}

extern "C" JNIEXPORT jlong JNICALL Java_org_nxemu_ui_main_NativeUi_create(JNIEnv * env, jobject /*thiz*/, jobject host, jboolean settings)
{
    const jlong handle = s_nextScreen++;
    auto screen = std::make_shared<std::unique_ptr<UiScreen>>(
        std::make_unique<UiScreen>(std::make_unique<UiHost>(env, host), settings ? UiActivity::Mode::Settings : UiActivity::Mode::Library));
    UiThread::Instance().Post([handle, screen] {
        if ((*screen)->Create())
        {
            s_screens[handle] = std::move(*screen);
        }
    });
    return handle;
}
