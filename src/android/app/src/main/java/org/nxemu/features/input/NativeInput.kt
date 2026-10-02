// SPDX-FileCopyrightText: 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.nxemu.features.input

import org.nxemu.features.input.model.ButtonName
import org.nxemu.features.input.model.NativeAnalog
import org.nxemu.features.input.model.NativeButton
import org.nxemu.features.input.model.NpadStyleIndex
import org.nxemu.utils.ParamPackage

object NativeInput {
    object ButtonState {
        const val RELEASED = 0
        const val PRESSED = 1
    }

    init {
        System.loadLibrary("nxemu-android")
    }

    external fun onGamePadButtonEvent(guid: String, port: Int, buttonId: Int, action: Int)

    external fun onGamePadAxisEvent(guid: String, port: Int, axis: Int, value: Float)
    external fun onTouchPressed(fingerId: Int, xAxis: Float, yAxis: Float)

    external fun onTouchMoved(fingerId: Int, xAxis: Float, yAxis: Float)

    external fun onTouchReleased(fingerId: Int)

    fun onOverlayButtonEvent(port: Int, button: NativeButton, action: Int) =
        onOverlayButtonEventImpl(port, button.int, action)

    private external fun onOverlayButtonEventImpl(port: Int, buttonId: Int, action: Int)

    fun onOverlayJoystickEvent(port: Int, stick: NativeAnalog, xAxis: Float, yAxis: Float) =
        onOverlayJoystickEventImpl(port, stick.int, xAxis, yAxis)

    private external fun onOverlayJoystickEventImpl(
        port: Int,
        stickId: Int,
        xAxis: Float,
        yAxis: Float,
    )

    external fun registerController(device: NxEmuInputDevice)

    external fun getInputDevices(): Array<String>

    external fun loadInputProfiles()

    external fun getInputProfileNames(): Array<String>

    external fun beginMapping(type: Int)

    external fun getNextInput(): String

    external fun stopMapping()

    fun updateMappingsWithDefault(
        playerIndex: Int,
        deviceParams: ParamPackage,
        displayName: String,
    ) = updateMappingsWithDefaultImpl(playerIndex, deviceParams.serialize(), displayName)

    private external fun updateMappingsWithDefaultImpl(
        playerIndex: Int,
        deviceParams: String,
        displayName: String,
    )

    fun getButtonParam(playerIndex: Int, button: NativeButton): ParamPackage =
        ParamPackage(getButtonParamImpl(playerIndex, button.int))

    private external fun getButtonParamImpl(playerIndex: Int, buttonId: Int): String

    fun setButtonParam(playerIndex: Int, button: NativeButton, param: ParamPackage) =
        setButtonParamImpl(playerIndex, button.int, param.serialize())

    private external fun setButtonParamImpl(playerIndex: Int, buttonId: Int, param: String)

    fun getStickParam(playerIndex: Int, stick: NativeAnalog): ParamPackage =
        ParamPackage(getStickParamImpl(playerIndex, stick.int))

    private external fun getStickParamImpl(playerIndex: Int, stickId: Int): String

    fun setStickParam(playerIndex: Int, stick: NativeAnalog, param: ParamPackage) =
        setStickParamImpl(playerIndex, stick.int, param.serialize())

    private external fun setStickParamImpl(playerIndex: Int, stickId: Int, param: String)

    fun getButtonName(param: ParamPackage): ButtonName =
        ButtonName.from(getButtonNameImpl(param.serialize()))

    private external fun getButtonNameImpl(param: String): Int

    fun getSupportedStyleTags(playerIndex: Int): List<NpadStyleIndex> =
        getSupportedStyleTagsImpl(playerIndex).map { NpadStyleIndex.from(it) }

    private external fun getSupportedStyleTagsImpl(playerIndex: Int): IntArray

    fun getStyleIndex(playerIndex: Int): NpadStyleIndex =
        NpadStyleIndex.from(getStyleIndexImpl(playerIndex))

    private external fun getStyleIndexImpl(playerIndex: Int): Int

    fun setStyleIndex(playerIndex: Int, style: NpadStyleIndex) =
        setStyleIndexImpl(playerIndex, style.int)

    private external fun setStyleIndexImpl(playerIndex: Int, styleIndex: Int)

    fun isController(params: ParamPackage): Boolean = isControllerImpl(params.serialize())

    private external fun isControllerImpl(params: String): Boolean

    external fun getIsConnected(playerIndex: Int): Boolean

    fun connectController(playerIndex: Int, connected: Boolean = true) {
        connectControllerImpl(playerIndex, connected)
    }

    private external fun connectControllerImpl(playerIndex: Int, connected: Boolean)

    external fun resetControllerMappings(playerIndex: Int)
}
