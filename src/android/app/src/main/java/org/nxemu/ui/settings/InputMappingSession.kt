package org.nxemu.ui.settings

import android.app.Activity
import android.app.AlertDialog
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import org.nxemu.R
import org.nxemu.features.input.NativeInput
import org.nxemu.features.input.NxEmuPhysicalDevice
import org.nxemu.features.input.model.AnalogDirection
import org.nxemu.features.input.model.InputType
import org.nxemu.features.input.model.NativeAnalog
import org.nxemu.features.input.model.NativeButton
import org.nxemu.utils.InputHandler
import org.nxemu.utils.InputHandler.getGUID
import org.nxemu.utils.ParamPackage

class InputMappingSession(
    private val activity: Activity,
    private val onApplied: () -> Unit,
) {
    private var dialog: AlertDialog? = null
    private var mappingAccepted = false
    private var mappingKind: Kind? = null
    private var playerIndex = 0
    private var filterIndex = 0

    fun isShowing(): Boolean = dialog?.isShowing == true

    fun dismiss() {
        val current = dialog
        dialog = null
        current?.setOnDismissListener(null)
        NativeInput.stopMapping()
        current?.dismiss()
    }

    fun onKey(event: KeyEvent): Boolean {
        if (!isShowing()) {
            return false
        }
        return onMappingKey(event)
    }

    fun onMotion(event: MotionEvent): Boolean {
        if (!isShowing()) {
            return false
        }
        return onMappingMotion(event)
    }

    fun begin(playerIndex: Int, mapId: String, title: String, filterIndex: Int) {
        val kind = kindFromId(mapId) ?: return
        begin(playerIndex, kind, title, filterIndex)
    }

    fun begin(playerIndex: Int, kind: Kind, title: String, filterIndex: Int) {
        dismiss()
        this.playerIndex = playerIndex
        this.filterIndex = filterIndex
        mappingAccepted = false
        mappingKind = kind
        InputHandler.updateControllerData()
        NativeInput.beginMapping(
            when (kind) {
                is Kind.Analog -> InputType.Stick.int
                else -> InputType.Button.int
            },
        )
        val message = if (kind is Kind.Analog) {
            activity.getString(R.string.stick_map_description)
        } else {
            activity.getString(R.string.button_map_description)
        }
        val prompt = AlertDialog.Builder(activity)
            .setTitle(activity.getString(R.string.map_control, title))
            .setMessage(message)
            .setNegativeButton(android.R.string.cancel) { _, _ -> NativeInput.stopMapping() }
            .setOnDismissListener {
                NativeInput.stopMapping()
                if (dialog != null) {
                    dialog = null
                }
            }
            .create()
        // AlertDialog is its own window; Activity.dispatchKeyEvent never sees pad input
        // while it is showing (face buttons click Cancel, D-pad moves focus).
        prompt.setOnKeyListener { _, _, event ->
            if (event.keyCode == KeyEvent.KEYCODE_BACK) {
                false
            } else {
                onMappingKey(event)
            }
        }
        prompt.setOnShowListener {
            prompt.window?.decorView?.apply {
                isFocusable = true
                isFocusableInTouchMode = true
                requestFocus()
                setOnGenericMotionListener { _, event -> onMappingMotion(event) }
            }
        }
        dialog = prompt
        prompt.show()
    }

    private fun kindFromId(mapId: String): Kind? {
        val parts = mapId.split(':')
        return when (parts.getOrNull(0)) {
            "button" -> Kind.Button(NativeButton.from(parts.getOrNull(1)?.toIntOrNull() ?: return null))
            "analog" -> Kind.Analog(
                NativeAnalog.from(parts.getOrNull(1)?.toIntOrNull() ?: return null),
                AnalogDirection.entries.firstOrNull { it.param == parts.getOrNull(2) } ?: return null,
            )
            "modifier" -> Kind.Modifier(NativeAnalog.from(parts.getOrNull(1)?.toIntOrNull() ?: return null))
            else -> null
        }
    }

    private fun isGamepadDevice(device: InputDevice?): Boolean {
        if (device == null) {
            return false
        }
        val sources = device.sources
        return sources and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
            sources and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK
    }

    private fun controllerFor(device: InputDevice): NxEmuPhysicalDevice? {
        InputHandler.androidControllers[device.controllerNumber]?.let { return it }
        InputHandler.updateControllerData()
        InputHandler.androidControllers[device.controllerNumber]?.let { return it }
        val guid = device.getGUID()
        return InputHandler.androidControllers.values.firstOrNull { it.getGUID() == guid }
    }

    private fun onMappingKey(event: KeyEvent): Boolean {
        val device = event.device ?: return false
        if (!isGamepadDevice(device)) {
            return false
        }
        val action = when (event.action) {
            KeyEvent.ACTION_DOWN -> NativeInput.ButtonState.PRESSED
            KeyEvent.ACTION_UP -> NativeInput.ButtonState.RELEASED
            else -> return false
        }
        val controllerData = controllerFor(device) ?: return false
        NativeInput.registerController(controllerData)
        NativeInput.onGamePadButtonEvent(
            controllerData.getGUID(),
            controllerData.getPort(),
            event.keyCode,
            action,
        )
        onInputReceived(device)
        return true
    }

    private fun onMappingMotion(event: MotionEvent): Boolean {
        val device = event.device ?: return false
        if (!isGamepadDevice(device)) {
            return false
        }
        val controllerData = controllerFor(device) ?: return false
        NativeInput.registerController(controllerData)
        event.device.motionRanges.forEach {
            NativeInput.onGamePadAxisEvent(
                controllerData.getGUID(),
                controllerData.getPort(),
                it.axis,
                event.getAxisValue(it.axis),
            )
            onInputReceived(device)
        }
        return true
    }

    private fun onInputReceived(device: InputDevice) {
        val params = ParamPackage(NativeInput.getNextInput())
        if (params.has("engine") && isInputAcceptable(params) && !mappingAccepted) {
            mappingAccepted = true
            NativeInput.stopMapping()
            params.set("display", "${device.name} ${params.get("port", 0)}")
            applyMapping(params)
            dialog?.setOnDismissListener(null)
            dialog?.dismiss()
            dialog = null
            onApplied()
        }
    }

    private fun applyMapping(params: ParamPackage) {
        when (val kind = mappingKind) {
            is Kind.Button -> {
                if ((kind.button == NativeButton.DUp || kind.button == NativeButton.DLeft) &&
                    params.has("axis")
                ) {
                    params.set("invert", "-")
                }
                NativeInput.setButtonParam(playerIndex, kind.button, params)
            }
            is Kind.Analog -> {
                var analogParam = NativeInput.getStickParam(playerIndex, kind.stick)
                analogParam = adjustAnalogParam(params, analogParam, kind.direction.param)
                analogParam.set("invert_y", "-")
                NativeInput.setStickParam(playerIndex, kind.stick, analogParam)
            }
            is Kind.Modifier -> {
                val analogParam = NativeInput.getStickParam(playerIndex, kind.stick)
                analogParam.set("modifier", params.serialize())
                NativeInput.setStickParam(playerIndex, kind.stick, analogParam)
            }
            null -> Unit
        }
    }

    private fun adjustAnalogParam(
        inputParam: ParamPackage,
        analogParam: ParamPackage,
        buttonName: String,
    ): ParamPackage {
        if (inputParam.has("axis_x") && inputParam.has("axis_y")) {
            return inputParam
        }
        if (!analogParam.has("engine") || analogParam.has("axis_x") || analogParam.has("axis_y")) {
            analogParam.clear()
            analogParam.set("engine", "analog_from_button")
        }
        analogParam.set(buttonName, inputParam.serialize())
        return analogParam
    }

    private fun isInputAcceptable(params: ParamPackage): Boolean {
        if (InputHandler.registeredControllers.size <= 1) {
            return true
        }
        val filtered = InputHandler.registeredControllers.filter { it.get("port", 0) != 100 }
        val currentDevice = filtered.getOrNull(filterIndex) ?: return true
        if (currentDevice.get("engine", "any") == "any") {
            return true
        }
        val guidMatch = params.get("guid", "") == currentDevice.get("guid", "") ||
            params.get("guid", "") == currentDevice.get("guid2", "")
        return params.get("engine", "") == currentDevice.get("engine", "") &&
            guidMatch &&
            params.get("port", 0) == currentDevice.get("port", 0)
    }

    sealed class Kind {
        data class Button(val button: NativeButton) : Kind()
        data class Analog(val stick: NativeAnalog, val direction: AnalogDirection) : Kind()
        data class Modifier(val stick: NativeAnalog) : Kind()
    }
}
