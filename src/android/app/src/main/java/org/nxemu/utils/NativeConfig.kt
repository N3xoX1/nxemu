package org.nxemu.utils

import org.json.JSONArray
import org.json.JSONObject
import org.nxemu.NXUISetting
import org.nxemu.NativeLibrary
import org.nxemu.overlay.model.OverlayControlData

object NativeConfig {
    fun getOverlayControlData(): Array<OverlayControlData> {
        val raw = NativeLibrary.getSettingString(NXUISetting.OverlayControlData)
        if (raw.isBlank()) {
            return emptyArray()
        }
        return try {
            val arr = JSONArray(raw)
            Array(arr.length()) { i ->
                val obj = arr.getJSONObject(i)
                OverlayControlData(
                    obj.getString("id"),
                    obj.optBoolean("enabled", true),
                    pairFrom(obj.optJSONArray("landscape")),
                    pairFrom(obj.optJSONArray("portrait")),
                    pairFrom(obj.optJSONArray("foldable")),
                )
            }
        } catch (_: Exception) {
            emptyArray()
        }
    }

    fun setOverlayControlData(overlayControlData: Array<OverlayControlData>) {
        val arr = JSONArray()
        overlayControlData.forEach { data ->
            arr.put(
                JSONObject().apply {
                    put("id", data.id)
                    put("enabled", data.enabled)
                    put("landscape", jsonPair(data.landscapePosition))
                    put("portrait", jsonPair(data.portraitPosition))
                    put("foldable", jsonPair(data.foldablePosition))
                },
            )
        }
        NativeLibrary.setSettingString(NXUISetting.OverlayControlData, arr.toString())
        NativeLibrary.saveSettings()
    }

    fun saveGlobalConfig() {
        NativeLibrary.saveSettings()
    }

    private fun pairFrom(arr: JSONArray?): Pair<Double, Double> {
        if (arr == null || arr.length() < 2) {
            return Pair(0.0, 0.0)
        }
        return Pair(arr.optDouble(0, 0.0), arr.optDouble(1, 0.0))
    }

    private fun jsonPair(position: Pair<Double, Double>): JSONArray =
        JSONArray().put(position.first).put(position.second)
}
