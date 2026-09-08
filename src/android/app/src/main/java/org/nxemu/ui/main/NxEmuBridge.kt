package org.nxemu.ui.main

import android.webkit.JavascriptInterface
import org.nxemu.GameLibraryScanner
import org.nxemu.NativeLibrary

class NxEmuBridge(private val activity: MainActivity) {
    @JavascriptInterface
    fun addGameDirectory() {
        activity.runOnUiThread { activity.AddGameDirectory() }
    }

    @JavascriptInterface
    fun setSettingString(setting: String, value: String) {
        NativeLibrary.setSettingString(setting, value)
    }

    @JavascriptInterface
    fun getSettingString(setting: String): String {
        return NativeLibrary.getSettingString(setting)
    }

    @JavascriptInterface
    fun saveSettings() {
        NativeLibrary.saveSettings()
    }

    @JavascriptInterface
    fun scanGameLibraryPaths(): String {
        return GameLibraryScanner.scanRomUris(activity.applicationContext)
    }

    @JavascriptInterface
    fun queryRomMetadata(path: String): String {
        return NativeLibrary.queryRomMetadata(path)
    }
}
