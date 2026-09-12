package org.nxemu.utils

import org.nxemu.overlay.model.OverlayControl
import org.nxemu.overlay.model.OverlayControlData

object NativeConfig {
    fun getOverlayControlData(): Array<OverlayControlData> =
        OverlayControl.entries.map { it.toOverlayControlData() }.toTypedArray()

    fun setOverlayControlData(overlayControlData: Array<OverlayControlData>) {
    }

    fun saveGlobalConfig() {
    }
}
