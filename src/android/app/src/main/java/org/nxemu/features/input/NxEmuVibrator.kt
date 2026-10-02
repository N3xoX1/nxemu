// SPDX-FileCopyrightText: 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.nxemu.features.input

import android.content.Context
import android.os.Build
import android.os.CombinedVibration
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.view.InputDevice
import androidx.annotation.Keep
import androidx.annotation.RequiresApi
import org.nxemu.NxEmuApplication

@Keep
@Suppress("DEPRECATION")
interface NxEmuVibrator {
    fun supportsVibration(): Boolean

    fun vibrate(intensity: Float)

    companion object {
        fun getControllerVibrator(device: InputDevice): NxEmuVibrator =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                NxEmuVibratorManager(device.vibratorManager)
            } else {
                NxEmuVibratorManagerCompat(device.vibrator)
            }

        fun getSystemVibrator(): NxEmuVibrator =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                val vibratorManager = NxEmuApplication.appContext
                    .getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as VibratorManager
                NxEmuVibratorManager(vibratorManager)
            } else {
                val vibrator = NxEmuApplication.appContext
                    .getSystemService(Context.VIBRATOR_SERVICE) as Vibrator
                NxEmuVibratorManagerCompat(vibrator)
            }

        fun getVibrationEffect(intensity: Float): VibrationEffect? {
            if (intensity > 0f) {
                return VibrationEffect.createOneShot(
                    50,
                    (255.0 * intensity).toInt().coerceIn(1, 255),
                )
            }
            return null
        }
    }
}

@RequiresApi(Build.VERSION_CODES.S)
class NxEmuVibratorManager(private val vibratorManager: VibratorManager) : NxEmuVibrator {
    override fun supportsVibration(): Boolean {
        return vibratorManager.vibratorIds.isNotEmpty()
    }

    override fun vibrate(intensity: Float) {
        val vibration = NxEmuVibrator.getVibrationEffect(intensity) ?: return
        vibratorManager.vibrate(CombinedVibration.createParallel(vibration))
    }
}

class NxEmuVibratorManagerCompat(private val vibrator: Vibrator) : NxEmuVibrator {
    override fun supportsVibration(): Boolean {
        return vibrator.hasVibrator()
    }

    override fun vibrate(intensity: Float) {
        val vibration = NxEmuVibrator.getVibrationEffect(intensity) ?: return
        vibrator.vibrate(vibration)
    }
}
