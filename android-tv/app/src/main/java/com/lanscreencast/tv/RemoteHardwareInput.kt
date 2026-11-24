package com.lanscreencast.tv

import android.hardware.input.InputManager
import android.os.Handler
import android.os.Looper
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import org.json.JSONObject
import kotlin.math.roundToInt

internal class RemoteHardwareInput(
    private val manager: InputManager,
    private val send: (JSONObject, Boolean) -> Unit,
    private val enabled: () -> Boolean,
) : InputManager.InputDeviceListener {
    private val handler = Handler(Looper.getMainLooper())
    private val keys = mutableMapOf<Pair<Int, Int>, Int>()
    private var mouseDevice: Int? = null
    private var buttons = 0
    private var pendingPosition: Pair<Int, Int>? = null
    private val move = Runnable { flushPosition() }

    fun register() = manager.registerInputDeviceListener(this, handler)

    fun close() {
        release()
        manager.unregisterInputDeviceListener(this)
    }

    override fun onInputDeviceAdded(deviceId: Int) = Unit
    override fun onInputDeviceChanged(deviceId: Int) = Unit
    override fun onInputDeviceRemoved(deviceId: Int) {
        if (mouseDevice == deviceId || keys.keys.any { it.first == deviceId }) release()
    }

    fun release() {
        clearPosition()
        if (keys.isNotEmpty() || buttons != 0) send(message("hardware_reset"), false)
        keys.clear()
        mouseDevice = null
        buttons = 0
    }

    fun key(event: KeyEvent): Boolean {
        if (!enabled() || !event.isFromSource(InputDevice.SOURCE_KEYBOARD)) return false
        val device = event.device ?: return false
        if (device.isVirtual || device.keyboardType != InputDevice.KEYBOARD_TYPE_ALPHABETIC) return false
        val scan = windowsScanCode(event.keyCode) ?: return false
        val id = event.deviceId to event.keyCode
        when (event.action) {
            KeyEvent.ACTION_DOWN -> {
                if (id in keys) {
                    if (event.repeatCount > 0 && !KeyEvent.isModifierKey(event.keyCode)) sendKey(scan, true, repeat = true)
                } else if (event.repeatCount == 0) {
                    if (scan !in keys.values) sendKey(scan, true)
                    keys[id] = scan
                }
            }
            KeyEvent.ACTION_UP -> {
                if (keys.remove(id) != null && scan !in keys.values) sendKey(scan, false)
            }
            else -> return false
        }
        return true
    }

    private fun sendKey(scan: Int, down: Boolean, repeat: Boolean = false) {
        send(message("key_state").put("scanCode", scan).put("down", down).put("repeat", repeat), repeat)
    }

    fun handlesMouse(event: MotionEvent) = enabled() && event.isFromSource(InputDevice.SOURCE_MOUSE)

    fun mouse(event: MotionEvent, locate: (clamp: Boolean) -> Pair<Int, Int>?): Boolean {
        if (buttons != 0 && mouseDevice != event.deviceId) return true
        val position = locate(buttons != 0)
        when (event.actionMasked) {
            MotionEvent.ACTION_HOVER_ENTER, MotionEvent.ACTION_HOVER_MOVE, MotionEvent.ACTION_MOVE -> {
                if (position != null) queuePosition(position)
                return true
            }
            MotionEvent.ACTION_HOVER_EXIT -> return true
            MotionEvent.ACTION_SCROLL -> {
                if (position == null) return true
                clearPosition()
                scroll(event.getAxisValue(MotionEvent.AXIS_VSCROLL), position, horizontal = false)
                scroll(event.getAxisValue(MotionEvent.AXIS_HSCROLL), position, horizontal = true)
                return true
            }
        }
        val reported = event.buttonState and mouseMask
        val next = when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> if (reported != 0) reported else MotionEvent.BUTTON_PRIMARY
            MotionEvent.ACTION_BUTTON_PRESS -> reported or (event.actionButton and mouseMask)
            MotionEvent.ACTION_BUTTON_RELEASE -> reported and event.actionButton.inv()
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> 0
            else -> return false
        }
        val changed = buttons xor next
        if (changed == 0 || (position == null && buttons == 0)) return true
        clearPosition()
        for ((bit, name) in mouseButtons) {
            if (changed and bit == 0) continue
            val down = next and bit != 0
            val packet = message("mouse_button").put("button", name).put("down", down)
            val location = if (event.actionMasked == MotionEvent.ACTION_CANCEL) null else position
            send(at(packet, location), false)
        }
        buttons = next
        mouseDevice = event.deviceId.takeIf { next != 0 }
        return true
    }

    private fun scroll(amount: Float, position: Pair<Int, Int>, horizontal: Boolean) {
        if (!amount.isFinite() || amount == 0f) return
        val delta = (amount.coerceIn(-8f, 8f) * 120f).roundToInt()
        if (delta == 0) return
        send(at(message("wheel").put("delta", delta).put("horizontal", horizontal), position), false)
    }

    private fun queuePosition(position: Pair<Int, Int>) {
        if (pendingPosition == null) handler.postDelayed(move, 12)
        pendingPosition = position
    }

    private fun flushPosition() {
        val position = pendingPosition ?: return
        clearPosition()
        if (enabled()) send(at(message("point"), position), true)
    }

    private fun clearPosition() {
        handler.removeCallbacks(move)
        pendingPosition = null
    }

    private fun at(packet: JSONObject, position: Pair<Int, Int>?) = packet.apply {
        position?.let { put("x", it.first).put("y", it.second) }
    }

    private fun message(type: String) = JSONObject().put("type", type)

    private companion object {
        const val mouseMask = MotionEvent.BUTTON_PRIMARY or MotionEvent.BUTTON_SECONDARY or MotionEvent.BUTTON_TERTIARY
        val mouseButtons = listOf(
            MotionEvent.BUTTON_PRIMARY to "left",
            MotionEvent.BUTTON_SECONDARY to "right",
            MotionEvent.BUTTON_TERTIARY to "middle",
        )
    }
}

// PC set 1 make codes. Bit 8 represents the E0 prefix used by extended keys.
internal fun windowsScanCode(key: Int): Int? = when (key) {
    in KeyEvent.KEYCODE_A..KeyEvent.KEYCODE_Z -> letterScanCodes[key - KeyEvent.KEYCODE_A]
    in KeyEvent.KEYCODE_1..KeyEvent.KEYCODE_9 -> 0x02 + key - KeyEvent.KEYCODE_1
    in KeyEvent.KEYCODE_F1..KeyEvent.KEYCODE_F10 -> 0x3b + key - KeyEvent.KEYCODE_F1
    in KeyEvent.KEYCODE_NUMPAD_0..KeyEvent.KEYCODE_NUMPAD_9 -> numpadScanCodes[key - KeyEvent.KEYCODE_NUMPAD_0]
    KeyEvent.KEYCODE_ESCAPE -> 0x01
    KeyEvent.KEYCODE_0 -> 0x0b
    KeyEvent.KEYCODE_MINUS -> 0x0c
    KeyEvent.KEYCODE_EQUALS -> 0x0d
    KeyEvent.KEYCODE_DEL -> 0x0e
    KeyEvent.KEYCODE_TAB -> 0x0f
    KeyEvent.KEYCODE_LEFT_BRACKET -> 0x1a
    KeyEvent.KEYCODE_RIGHT_BRACKET -> 0x1b
    KeyEvent.KEYCODE_ENTER -> 0x1c
    KeyEvent.KEYCODE_CTRL_LEFT -> 0x1d
    KeyEvent.KEYCODE_SEMICOLON -> 0x27
    KeyEvent.KEYCODE_APOSTROPHE -> 0x28
    KeyEvent.KEYCODE_GRAVE -> 0x29
    KeyEvent.KEYCODE_SHIFT_LEFT -> 0x2a
    KeyEvent.KEYCODE_BACKSLASH -> 0x2b
    KeyEvent.KEYCODE_COMMA -> 0x33
    KeyEvent.KEYCODE_PERIOD -> 0x34
    KeyEvent.KEYCODE_SLASH -> 0x35
    KeyEvent.KEYCODE_SHIFT_RIGHT -> 0x36
    KeyEvent.KEYCODE_NUMPAD_MULTIPLY -> 0x37
    KeyEvent.KEYCODE_ALT_LEFT -> 0x38
    KeyEvent.KEYCODE_SPACE -> 0x39
    KeyEvent.KEYCODE_CAPS_LOCK -> 0x3a
    KeyEvent.KEYCODE_NUM_LOCK -> 0x45
    KeyEvent.KEYCODE_SCROLL_LOCK -> 0x46
    KeyEvent.KEYCODE_NUMPAD_SUBTRACT -> 0x4a
    KeyEvent.KEYCODE_NUMPAD_ADD -> 0x4e
    KeyEvent.KEYCODE_NUMPAD_DOT, KeyEvent.KEYCODE_NUMPAD_COMMA -> 0x53
    KeyEvent.KEYCODE_F11 -> 0x57
    KeyEvent.KEYCODE_F12 -> 0x58
    KeyEvent.KEYCODE_NUMPAD_ENTER -> 0x11c
    KeyEvent.KEYCODE_CTRL_RIGHT -> 0x11d
    KeyEvent.KEYCODE_NUMPAD_DIVIDE -> 0x135
    KeyEvent.KEYCODE_SYSRQ -> 0x137
    KeyEvent.KEYCODE_ALT_RIGHT -> 0x138
    KeyEvent.KEYCODE_MOVE_HOME -> 0x147
    KeyEvent.KEYCODE_DPAD_UP -> 0x148
    KeyEvent.KEYCODE_PAGE_UP -> 0x149
    KeyEvent.KEYCODE_DPAD_LEFT -> 0x14b
    KeyEvent.KEYCODE_DPAD_RIGHT -> 0x14d
    KeyEvent.KEYCODE_MOVE_END -> 0x14f
    KeyEvent.KEYCODE_DPAD_DOWN -> 0x150
    KeyEvent.KEYCODE_PAGE_DOWN -> 0x151
    KeyEvent.KEYCODE_INSERT -> 0x152
    KeyEvent.KEYCODE_FORWARD_DEL -> 0x153
    KeyEvent.KEYCODE_META_LEFT -> 0x15b
    KeyEvent.KEYCODE_META_RIGHT -> 0x15c
    KeyEvent.KEYCODE_MENU -> 0x15d
    else -> null
}

private val letterScanCodes = intArrayOf(
    0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
    0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c,
)
private val numpadScanCodes = intArrayOf(0x52, 0x4f, 0x50, 0x51, 0x4b, 0x4c, 0x4d, 0x47, 0x48, 0x49)
