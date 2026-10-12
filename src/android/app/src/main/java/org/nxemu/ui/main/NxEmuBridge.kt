package org.nxemu.ui.main

import android.content.Context
import android.content.Intent
import android.webkit.JavascriptInterface
import org.json.JSONArray
import org.nxemu.GameLibraryScanner
import org.nxemu.NativeLibrary
import org.nxemu.ui.settings.ControllerSettings
import org.nxemu.utils.GpuDriverFetcher
import org.nxemu.utils.GpuDriverHelper
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
    fun setStickValue(playerIndex: Int, stickId: Int, key: String, value: Double) {
        ControllerSettings.setStickValue(playerIndex, stickId, key, value.toFloat())
    }

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
    fun gpuDriverList(): String = GpuDriverHelper.listDriversJson()

    @JavascriptInterface
    fun installGpuDriver() {
        activity.runOnUiThread { activity.installGpuDriver() }
    }

    @JavascriptInterface
    fun selectGpuDriver(fileName: String) {
        Thread({
            if (fileName.isEmpty()) {
                GpuDriverHelper.useSystemDriver()
            } else {
                GpuDriverHelper.useStoredDriver(fileName)
            }
            activity.runOnUiThread { activity.refreshGpuDrivers() }
        }, "GpuDriverSelect").start()
    }

    @JavascriptInterface
    fun gpuDriverRecommendation(): String = GpuDriverFetcher.recommendedLabel()

    @JavascriptInterface
    fun fetchGpuDriverCatalog(force: Boolean) {
        Thread({
            val json = try {
                GpuDriverFetcher.catalogJson(force)
            } catch (_: Exception) {
                "{\"error\":\"Couldn't load the driver list\"}"
            }
            activity.dispatchGpuDriverEvent("onGpuDriverCatalog", json)
        }, "GpuDriverCatalog").start()
    }

    @JavascriptInterface
    fun downloadGpuDriver(repo: String, tag: String, asset: String) {
        Thread({
            val result = GpuDriverFetcher.download(repo, tag, asset) { percent ->
                activity.dispatchGpuDriverEvent("onGpuDriverDownload", "{\"percent\":$percent}")
            }
            activity.dispatchGpuDriverEvent("onGpuDriverDownload", "{\"result\":\"$result\"}")
            activity.runOnUiThread { activity.showGpuDriverResult(result) }
        }, "GpuDriverDownload").start()
    }

    @JavascriptInterface
    fun removeGpuDriver(fileName: String) {
        Thread({
            GpuDriverHelper.removeDriver(fileName)
            activity.runOnUiThread { activity.refreshGpuDrivers() }
        }, "GpuDriverRemove").start()
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
    fun openSettings() {
        activity.runOnUiThread {
            if (activity is SettingsActivity) {
                return@runOnUiThread
            }
            activity.startActivity(Intent(activity, SettingsActivity::class.java))
        }
    }

    @JavascriptInterface
    fun closeSettings() {
        activity.runOnUiThread {
            if (activity is SettingsActivity) {
                activity.finish()
            }
        }
    }

    @JavascriptInterface
    fun isSetupComplete(): Boolean {
        return activity.getSharedPreferences(UI_PREFS, Context.MODE_PRIVATE).getBoolean(SETUP_COMPLETE, false)
    }

    @JavascriptInterface
    fun completeSetup() {
        activity.getSharedPreferences(UI_PREFS, Context.MODE_PRIVATE).edit().putBoolean(SETUP_COMPLETE, true).apply()
    }

    @JavascriptInterface
    fun launchGame(path: String) {
        activity.runOnUiThread { activity.launchGame(path) }
    }

    companion object {
        private const val UI_PREFS = "nxemu_ui"
        private const val SETUP_COMPLETE = "setup_complete"
    }
}
