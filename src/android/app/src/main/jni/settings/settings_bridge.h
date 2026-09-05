#pragma once
#include <jni.h>

void SettingsChange_Start(JavaVM * javaVm, JNIEnv * env);
void SettingsChange_Stop(JNIEnv * env);
