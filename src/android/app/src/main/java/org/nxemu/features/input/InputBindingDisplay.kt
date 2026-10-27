package org.nxemu.features.input

import org.nxemu.features.input.model.ButtonName
import org.nxemu.utils.ParamPackage
import org.nxemu.utils.Translation.tr

object InputBindingDisplay {
    fun buttonToText(param: ParamPackage): String {
        if (!param.has("engine")) {
            return tr("Not set")
        }

        val toggle = if (param.get("toggle", false)) "~" else ""
        val inverted = if (param.get("inverted", false)) "!" else ""
        val invert = if (param.get("invert", "+") == "-") "-" else ""
        val turbo = if (param.get("turbo", false)) "$" else ""
        val commonButtonName = NativeInput.getButtonName(param)

        if (commonButtonName == ButtonName.Invalid) {
            return tr("Invalid")
        }
        if (commonButtonName == ButtonName.Engine) {
            return param.get("engine", "")
        }
        if (commonButtonName == ButtonName.Value) {
            if (param.has("hat")) {
                val hat = directionName(param.get("direction", ""))
                return turbo + toggle + inverted + tr("Hat %1", hat)
            }
            if (param.has("axis")) {
                val axis = param.get("axis", "")
                return toggle + inverted + invert + tr("Axis %1", axis)
            }
            if (param.has("button")) {
                val button = param.get("button", "")
                return turbo + toggle + inverted + tr("Button %1", button)
            }
        }
        return tr("Unknown")
    }

    fun analogToText(param: ParamPackage, direction: String): String {
        if (!param.has("engine")) {
            return tr("Not set")
        }
        if (param.get("engine", "") == "analog_from_button") {
            return buttonToText(ParamPackage(param.get(direction, "")))
        }
        if (!param.has("axis_x") || !param.has("axis_y")) {
            return tr("Unknown")
        }

        val xAxis = param.get("axis_x", "")
        val yAxis = param.get("axis_y", "")
        val xInvert = param.get("invert_x", "+") == "-"
        val yInvert = param.get("invert_y", "+") == "-"

        if (direction == "modifier") {
            return tr("Unused")
        }

        return when (direction) {
            "up" -> tr("Axis %1", yAxis + if (yInvert) "+" else "-")
            "down" -> tr("Axis %1", yAxis + if (yInvert) "-" else "+")
            "left" -> tr("Axis %1", xAxis + if (xInvert) "+" else "-")
            "right" -> tr("Axis %1", xAxis + if (xInvert) "-" else "+")
            else -> tr("Unknown")
        }
    }

    fun getDisplayString(params: ParamPackage, control: String): String {
        val deviceName = params.get("display", "")
        if (deviceName.isEmpty()) {
            return tr("Not set")
        }
        return "$deviceName: $control"
    }

    private fun directionName(direction: String): String {
        return when (direction) {
            "up" -> tr("Up")
            "down" -> tr("Down")
            "left" -> tr("Left")
            "right" -> tr("Right")
            else -> direction
        }
    }
}
