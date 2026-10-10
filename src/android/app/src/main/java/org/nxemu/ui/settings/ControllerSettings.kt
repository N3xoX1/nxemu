package org.nxemu.ui.settings

import android.content.Context
import org.json.JSONArray
import org.nxemu.R
import org.nxemu.features.input.InputBindingDisplay
import org.nxemu.features.input.NativeInput
import org.nxemu.features.input.model.NativeAnalog
import org.nxemu.features.input.model.NativeButton
import org.nxemu.utils.InputHandler

object ControllerSettings {
    fun isConnected(playerIndex: Int): Boolean = NativeInput.getIsConnected(player(playerIndex))

    fun setConnected(playerIndex: Int, connected: Boolean) {
        NativeInput.connectController(player(playerIndex), connected)
    }

    fun style(playerIndex: Int): Int = NativeInput.getStyleIndex(player(playerIndex)).int

    fun supportedStyles(playerIndex: Int): String {
        val styles = JSONArray()
        NativeInput.getSupportedStyleTags(player(playerIndex)).forEach { style ->
            styles.put(style.int)
        }
        return styles.toString()
    }

    fun setStyle(playerIndex: Int, style: Int) {
        val match = NativeInput.getSupportedStyleTags(player(playerIndex)).firstOrNull { it.int == style } ?: return
        NativeInput.setStyleIndex(player(playerIndex), match)
    }

    fun buttonBinding(playerIndex: Int, buttonId: Int): String {
        val params = NativeInput.getButtonParam(player(playerIndex), NativeButton.from(buttonId))
        return InputBindingDisplay.getDisplayString(params, InputBindingDisplay.buttonToText(params))
    }

    fun stickBinding(playerIndex: Int, stickId: Int, part: String): String {
        val params = NativeInput.getStickParam(player(playerIndex), NativeAnalog.from(stickId))
        val text = InputBindingDisplay.analogToText(params, part)
        return if (part == "modifier") text else InputBindingDisplay.getDisplayString(params, text)
    }

    fun isControllerStick(playerIndex: Int, stickId: Int): Boolean {
        val params = NativeInput.getStickParam(player(playerIndex), NativeAnalog.from(stickId))
        return NativeInput.isController(params)
    }

    fun stickValue(playerIndex: Int, stickId: Int, key: String): Float {
        val params = NativeInput.getStickParam(player(playerIndex), NativeAnalog.from(stickId))
        return params.get(key, stickDefault(key))
    }

    fun setStickValue(playerIndex: Int, stickId: Int, key: String, value: Float) {
        if (key != "range" && key != "deadzone" && key != "modifier_scale") {
            return
        }
        val stick = NativeAnalog.from(stickId)
        val next = NativeInput.getStickParam(player(playerIndex), stick)
        next.set(key, value)
        NativeInput.setStickParam(player(playerIndex), stick, next)
    }

    fun resetMappings(playerIndex: Int) {
        NativeInput.resetControllerMappings(player(playerIndex))
    }

    fun filterNames(context: Context): String {
        InputHandler.updateControllerData()
        return JSONArray(deviceNames(context, includeAny = true)).toString()
    }

    fun autoMapNames(context: Context): String {
        InputHandler.updateControllerData()
        return JSONArray(deviceNames(context, includeAny = false)).toString()
    }

    fun autoMap(context: Context, playerIndex: Int, index: Int) {
        InputHandler.updateControllerData()
        val controllers = InputHandler.registeredControllers.toList()
        val unknownName = context.getString(R.string.unknown)
        val filtered = controllers.filter {
            val port = it.get("port", -1)
            port != 100 && port != -1
        }
        val byYuzuIndex = controllers.getOrNull(index + 1)
        val yuzuPort = byYuzuIndex?.get("port", -1) ?: -1
        val device = if (byYuzuIndex != null && yuzuPort != 100 && yuzuPort != -1) {
            byYuzuIndex
        } else {
            filtered.getOrNull(index)
        } ?: return
        NativeInput.updateMappingsWithDefault(
            player(playerIndex),
            device,
            device.get("display", unknownName),
        )
    }

    private fun deviceNames(context: Context, includeAny: Boolean): List<String> {
        val unknownName = context.getString(R.string.unknown)
        return InputHandler.registeredControllers.mapNotNull { params ->
            val port = params.get("port", -1)
            if (port == 100 || (!includeAny && port == -1)) {
                null
            } else {
                params.get("display", unknownName)
            }
        }
    }

    private fun stickDefault(key: String): Float {
        return when (key) {
            "range" -> 0.95f
            "deadzone" -> 0.15f
            "modifier_scale" -> 0.5f
            else -> 0f
        }
    }

    private fun player(playerIndex: Int): Int = playerIndex.coerceIn(0, 7)
}
