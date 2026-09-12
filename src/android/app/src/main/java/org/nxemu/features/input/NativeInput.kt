// SPDX-FileCopyrightText: 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.nxemu.features.input

import org.nxemu.features.input.model.NativeAnalog
import org.nxemu.features.input.model.NativeButton
import org.nxemu.features.input.model.NpadStyleIndex

object NativeInput {
    init {
        System.loadLibrary("nxemu-android")
    }

    object ButtonState {
        const val RELEASED = 0
        const val PRESSED = 1
    }

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

    fun getStyleIndex(playerIndex: Int): NpadStyleIndex =
        NpadStyleIndex.from(getStyleIndexImpl(playerIndex))

    private external fun getStyleIndexImpl(playerIndex: Int): Int
}
