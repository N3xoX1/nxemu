// SPDX-FileCopyrightText: 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.nxemu.features.input

import android.view.InputDevice
import androidx.annotation.Keep
import org.nxemu.utils.InputHandler.getGUID

@Keep
interface NxEmuInputDevice {
    fun getName(): String

    fun getGUID(): String

    fun getPort(): Int

    fun getSupportsVibration(): Boolean

    fun vibrate(intensity: Float)

    fun getAxes(): Array<Int> = arrayOf()

    fun hasKeys(keys: IntArray): BooleanArray = BooleanArray(0)
}

class NxEmuPhysicalDevice(
    private val device: InputDevice,
    private val port: Int,
    useSystemVibrator: Boolean,
) : NxEmuInputDevice {
    private val vibrator = if (useSystemVibrator) {
        NxEmuVibrator.getSystemVibrator()
    } else {
        NxEmuVibrator.getControllerVibrator(device)
    }

    override fun getName(): String = device.name

    override fun getGUID(): String = device.getGUID()

    override fun getPort(): Int = port

    override fun getSupportsVibration(): Boolean = vibrator.supportsVibration()

    override fun vibrate(intensity: Float) {
        vibrator.vibrate(intensity)
    }

    override fun getAxes(): Array<Int> = device.motionRanges.map { it.axis }.toTypedArray()

    override fun hasKeys(keys: IntArray): BooleanArray = device.hasKeys(*keys)
}

class NxEmuInputOverlayDevice(
    private val vibration: Boolean,
    private val port: Int,
) : NxEmuInputDevice {
    private val vibrator = NxEmuVibrator.getSystemVibrator()

    override fun getName(): String = "Touch controls"

    override fun getGUID(): String = "00000000000000000000000000000000"

    override fun getPort(): Int = port

    override fun getSupportsVibration(): Boolean = vibration && vibrator.supportsVibration()

    override fun vibrate(intensity: Float) {
        if (vibration) {
            vibrator.vibrate(intensity)
        }
    }
}
