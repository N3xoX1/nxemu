package org.nxemu.ui.main

import android.webkit.JavascriptInterface
import org.json.JSONArray
import org.nxemu.GameLibraryScanner
import org.nxemu.NativeLibrary
import org.nxemu.ui.settings.ControllerSettings
import org.nxemu.utils.InputHandler
import org.nxemu.utils.ThemeHelper

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
    fun getSettingBool(setting: String): Boolean {
        return NativeLibrary.getSettingBool(setting)
    }

    @JavascriptInterface
    fun setSettingBool(setting: String, value: Boolean) {
        NativeLibrary.setSettingBool(setting, value)
    }

    @JavascriptInterface
    fun getSettingInt(setting: String): Int {
        return NativeLibrary.getSettingInt(setting)
    }

    @JavascriptInterface
    fun setSettingInt(setting: String, value: Int) {
        NativeLibrary.setSettingInt(setting, value)
    }

    @JavascriptInterface
    fun isControllerConnected(playerIndex: Int): Boolean = ControllerSettings.isConnected(playerIndex)

    @JavascriptInterface
    fun setControllerConnected(playerIndex: Int, connected: Boolean) {
        ControllerSettings.setConnected(playerIndex, connected)
    }

    @JavascriptInterface
    fun getControllerStyle(playerIndex: Int): Int = ControllerSettings.style(playerIndex)

    @JavascriptInterface
    fun getSupportedControllerStyles(playerIndex: Int): String =
        ControllerSettings.supportedStyles(playerIndex)

    @JavascriptInterface
    fun setControllerStyle(playerIndex: Int, style: Int) {
        ControllerSettings.setStyle(playerIndex, style)
    }

    @JavascriptInterface
    fun getButtonBinding(playerIndex: Int, buttonId: Int): String =
        ControllerSettings.buttonBinding(playerIndex, buttonId)

    @JavascriptInterface
    fun getStickBinding(playerIndex: Int, stickId: Int, part: String): String =
        ControllerSettings.stickBinding(playerIndex, stickId, part)

    @JavascriptInterface
    fun isControllerStick(playerIndex: Int, stickId: Int): Boolean =
        ControllerSettings.isControllerStick(playerIndex, stickId)

    @JavascriptInterface
    fun getStickValue(playerIndex: Int, stickId: Int, key: String): Float =
        ControllerSettings.stickValue(playerIndex, stickId, key)

    @JavascriptInterface
    fun resetControllerMappings(playerIndex: Int) {
        ControllerSettings.resetMappings(playerIndex)
    }

    @JavascriptInterface
    fun getControllerFilterNames(): String = ControllerSettings.filterNames(activity)

    @JavascriptInterface
    fun getAutoMapControllerNames(): String = ControllerSettings.autoMapNames(activity)

    @JavascriptInterface
    fun autoMapController(playerIndex: Int, index: Int) {
        ControllerSettings.autoMap(activity, playerIndex, index)
    }

    @JavascriptInterface
    fun beginControllerMap(playerIndex: Int, mapId: String, title: String, filterIndex: Int) {
        activity.beginControllerMap(playerIndex, mapId, title, filterIndex)
    }

    @JavascriptInterface
    fun connectedControllerNames(): String {
        val names = InputHandler.getDevices().values.map { it.getName() }
        return JSONArray(names).toString()
    }

    @JavascriptInterface
    fun editOverlayLayout() {
        activity.runOnUiThread { activity.editOverlayLayout() }
    }

    @JavascriptInterface
    fun isDarkTheme(): Boolean {
        return ThemeHelper.isDark(activity)
    }

    @JavascriptInterface
    fun requestGameLibraryScan(gen: Int) {
        Thread({
            val json = GameLibraryScanner.scanRomUris(activity.applicationContext)
            activity.runOnUiThread {
                if (!activity.isDestroyed) {
                    activity.dispatchGameLibraryPaths(gen, json)
                }
            }
        }, "GameLibraryScan").start()
    }

    @JavascriptInterface
    fun queryRomMetadata(path: String): String {
        return NativeLibrary.queryRomMetadata(path)
    }

    @JavascriptInterface
    fun launchGame(path: String) {
        activity.runOnUiThread { activity.launchGame(path) }
    }
}
