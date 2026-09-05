package org.nxemu

import android.util.Log
import org.json.JSONArray

object NativeLibrary {
    init {
        System.loadLibrary("nxemu-android")
    }

    var onSettingChangedListener: ((String) -> Unit)? = null

    @JvmStatic
    fun onSettingChanged(setting: String) {
        Log.d("NxEmu", "onSettingChanged (kotlin): $setting")
        onSettingChangedListener?.invoke(setting)
    }

    @JvmStatic
    fun restorePersistedGameDirectoryAccess() {
        val persisted = NxEmuApplication.appContext.contentResolver.persistedUriPermissions
            .map { it.uri.toString() }
            .toSet()

        val existing = getSettingString(NXUISetting.GameDirectories)
        val dirs = try {
            JSONArray(existing)
        } catch (_: Exception) {
            JSONArray()
        }
        val kept = JSONArray()
        var changed = false
        for (i in 0 until dirs.length()) {
            val path = dirs.getString(i)
            val isContent = path.startsWith("content:", ignoreCase = true)
            if (!isContent || persisted.contains(path)) {
                kept.put(path)
            } else {
                changed = true
                Log.d("NxEmu", "Dropped game directory without persisted access: $path")
            }
        }
        if (changed) {
            setSettingString(NXUISetting.GameDirectories, kept.toString())
            saveSettings()
        }
    }

    @JvmStatic
    external fun appInit(appDirectory: String, nativeModuleLibDir: String)

    @JvmStatic
    external fun appCleanup()

    @JvmStatic
    external fun getSettingString(setting: String): String

    @JvmStatic
    external fun setSettingString(setting: String, value: String)

    @JvmStatic
    external fun saveSettings()
}
