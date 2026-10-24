package org.nxemu.ui.main

import android.content.Context
import android.content.Intent
import androidx.annotation.Keep
import org.json.JSONArray
import org.nxemu.GameLibraryScanner
import org.nxemu.ui.settings.ControllerSettings
import org.nxemu.utils.GpuDriverFetcher
import org.nxemu.utils.GpuDriverHelper
import org.nxemu.utils.InputHandler
import org.nxemu.utils.ThemeHelper

class UiHost(private val activity: MainActivity) {
    @Keep
    fun request(name: String, args: Array<String>): String {
        return when (name) {
            "addGameDirectory" -> ui { activity.addGameDirectory() }
            "openSettings" -> ui {
                if (activity !is SettingsActivity) {
                    activity.startActivity(Intent(activity, SettingsActivity::class.java))
                }
            }
            "finish" -> ui { activity.finish() }
            "launchGame" -> ui { activity.launchGame(args[0]) }
            "confirm" -> ui { activity.confirm(args[0].toInt(), args[1]) }
            "alert" -> ui { activity.alert(args[0]) }
            "editOverlayLayout" -> ui { activity.editOverlayLayout() }
            "isDarkTheme" -> ThemeHelper.isDark(activity).toString()
            "isSetupComplete" -> prefs().getBoolean(SETUP_COMPLETE, false).toString()
            "completeSetup" -> {
                prefs().edit().putBoolean(SETUP_COMPLETE, true).apply()
                ""
            }
            "requestGameLibraryScan" -> {
                val gen = args[0].toInt()
                Thread({
                    activity.dispatchGameLibraryPaths(gen, GameLibraryScanner.scanRomUris(activity.applicationContext))
                }, "GameLibraryScan").start()
                ""
            }

            "connectedControllerNames" -> JSONArray(InputHandler.getDevices().values.map { it.getName() }).toString()
            "isControllerConnected" -> ControllerSettings.isConnected(args[0].toInt()).toString()
            "setControllerConnected" -> {
                ControllerSettings.setConnected(args[0].toInt(), args[1].toBoolean())
                ""
            }
            "getControllerStyle" -> ControllerSettings.style(args[0].toInt()).toString()
            "getSupportedControllerStyles" -> ControllerSettings.supportedStyles(args[0].toInt())
            "setControllerStyle" -> {
                ControllerSettings.setStyle(args[0].toInt(), args[1].toInt())
                ""
            }
            "getButtonBinding" -> ControllerSettings.buttonBinding(args[0].toInt(), args[1].toInt())
            "getStickBinding" -> ControllerSettings.stickBinding(args[0].toInt(), args[1].toInt(), args[2])
            "isControllerStick" -> ControllerSettings.isControllerStick(args[0].toInt(), args[1].toInt()).toString()
            "getStickValue" -> ControllerSettings.stickValue(args[0].toInt(), args[1].toInt(), args[2]).toString()
            "setStickValue" -> {
                ControllerSettings.setStickValue(args[0].toInt(), args[1].toInt(), args[2], args[3].toFloat())
                ""
            }
            "resetControllerMappings" -> {
                ControllerSettings.resetMappings(args[0].toInt())
                ""
            }
            "getControllerFilterNames" -> ControllerSettings.filterNames(activity)
            "getAutoMapControllerNames" -> ControllerSettings.autoMapNames(activity)
            "autoMapController" -> {
                ControllerSettings.autoMap(activity, args[0].toInt(), args[1].toInt())
                ""
            }
            "beginControllerMap" -> {
                activity.beginControllerMap(args[0].toInt(), args[1], args[2], args[3].toInt())
                ""
            }

            "gpuDriverList" -> GpuDriverHelper.listDriversJson()
            "gpuDriverRecommendation" -> GpuDriverFetcher.recommendedLabel()
            "installGpuDriver" -> ui { activity.installGpuDriver() }
            "selectGpuDriver" -> background("GpuDriverSelect") {
                if (args[0].isEmpty()) {
                    GpuDriverHelper.useSystemDriver()
                } else {
                    GpuDriverHelper.useStoredDriver(args[0])
                }
                activity.refreshGpuDrivers()
            }
            "removeGpuDriver" -> background("GpuDriverRemove") {
                GpuDriverHelper.removeDriver(args[0])
                activity.refreshGpuDrivers()
            }
            "fetchGpuDriverCatalog" -> background("GpuDriverCatalog") {
                val json = try {
                    GpuDriverFetcher.catalogJson(args[0].toBoolean())
                } catch (_: Exception) {
                    "{\"error\":\"Couldn't load the driver list\"}"
                }
                activity.dispatchUiEvent("gpuCatalog", json)
            }
            "downloadGpuDriver" -> background("GpuDriverDownload") {
                val result = GpuDriverFetcher.download(args[0], args[1], args[2]) { percent ->
                    activity.dispatchUiEvent("gpuDownload", "{\"percent\":$percent}")
                }
                activity.dispatchUiEvent("gpuDownload", "{\"result\":\"$result\"}")
                activity.runOnUiThread { activity.showGpuDriverResult(result) }
            }

            else -> ""
        }
    }

    private fun ui(work: () -> Unit): String {
        activity.runOnUiThread(work)
        return ""
    }

    private fun background(name: String, work: () -> Unit): String {
        Thread(work, name).start()
        return ""
    }

    private fun prefs() = activity.getSharedPreferences(UI_PREFS, Context.MODE_PRIVATE)

    companion object {
        private const val UI_PREFS = "nxemu_ui"
        private const val SETUP_COMPLETE = "setup_complete"
    }
}
