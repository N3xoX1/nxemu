// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.nxemu.utils

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import org.nxemu.features.input.NativeInput
import org.nxemu.features.input.NxEmuInputOverlayDevice
import org.nxemu.features.input.NxEmuPhysicalDevice

object InputHandler {
    var androidControllers = mapOf<Int, NxEmuPhysicalDevice>()
    var registeredControllers = mutableListOf<ParamPackage>()

    fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val action = when (event.action) {
            KeyEvent.ACTION_DOWN -> NativeInput.ButtonState.PRESSED
            KeyEvent.ACTION_UP -> NativeInput.ButtonState.RELEASED
            else -> return false
        }

        var controllerData = androidControllers[event.device.controllerNumber]
        if (controllerData == null) {
            updateControllerData()
            controllerData = androidControllers[event.device.controllerNumber] ?: return false
        }

        NativeInput.registerController(controllerData)
        NativeInput.onGamePadButtonEvent(
            controllerData.getGUID(),
            controllerData.getPort(),
            event.keyCode,
            action,
        )
        return true
    }

    fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        val controllerData = androidControllers[event.device.controllerNumber] ?: return false
        NativeInput.registerController(controllerData)
        event.device.motionRanges.forEach {
            NativeInput.onGamePadAxisEvent(
                controllerData.getGUID(),
                controllerData.getPort(),
                it.axis,
                event.getAxisValue(it.axis),
            )
        }
        return true
    }

    fun getDevices(): Map<Int, NxEmuPhysicalDevice> {
        val gameControllerDeviceIds = mutableMapOf<Int, NxEmuPhysicalDevice>()
        val deviceIds = InputDevice.getDeviceIds()
        var port = 0
        deviceIds.forEach { deviceId ->
            InputDevice.getDevice(deviceId)?.apply {
                if (sources and InputDevice.SOURCE_GAMEPAD == InputDevice.SOURCE_GAMEPAD ||
                    sources and InputDevice.SOURCE_JOYSTICK == InputDevice.SOURCE_JOYSTICK
                ) {
                    if (!gameControllerDeviceIds.contains(controllerNumber)) {
                        gameControllerDeviceIds[controllerNumber] = NxEmuPhysicalDevice(
                            this,
                            port,
                            useSystemVibrator = port == 0,
                        )
                    }
                    port++
                }
            }
        }
        return gameControllerDeviceIds
    }

    fun updateControllerData() {
        androidControllers = getDevices()
        androidControllers.forEach {
            NativeInput.registerController(it.value)
        }
        NativeInput.registerController(NxEmuInputOverlayDevice(androidControllers.isEmpty(), 100))
        registeredControllers.clear()
        NativeInput.getInputDevices().forEach {
            registeredControllers.add(ParamPackage(it))
        }
        registeredControllers.sortBy { it.get("port", 0) }
    }

    fun InputDevice.getGUID(): String = String.format("%016x%016x", productId, vendorId)
}
