package com.lanscreencast.tv

import android.hardware.input.InputManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import org.json.JSONObject
import kotlin.math.abs
import kotlin.math.roundToInt

internal class ControllerInput(
    private val manager: InputManager,
    private val send: (JSONObject, Boolean) -> Unit,
    private val gameAxes: (GamepadAxes) -> Unit,
    private val gameButtons: (Int) -> Unit,
    private val onKeyboard: (Int) -> Unit,
    private val openKeyboard: () -> Unit,
    private val startGame: () -> Unit,
    private val stopGame: () -> Unit,
    private val onDevice: (String?) -> Unit,
) : InputManager.InputDeviceListener {
    enum class Target { NONE, DESKTOP, KEYBOARD, GAME, TRANSITION }

    private val handler = Handler(Looper.getMainLooper())
    private var device: InputDevice? = null
    private var target = Target.NONE
    private var axes = GamepadAxes()
    private var buttons = 0
    private var hat = 0
    private var digitalLeftTrigger = false
    private var digitalRightTrigger = false
    private var lastButtons = 0
    private var dragging = false
    private var wheel = 0f
    private var escapeSince = 0L

    private val frame = object : Runnable {
        override fun run() {
            when (target) {
                Target.DESKTOP -> moveDesktop()
                Target.GAME -> if (escapeSince != 0L &&
                    SystemClock.uptimeMillis() - escapeSince >= 650
                ) {
                    escapeSince = 0
                    stopGame()
                }
                else -> return
            }
            handler.postDelayed(this, 16)
        }
    }

    fun register() {
        manager.registerInputDeviceListener(this, handler)
        refreshDevice()
    }

    fun close() {
        setTarget(Target.NONE)
        manager.unregisterInputDeviceListener(this)
    }

    fun setTarget(next: Target) {
        if (next == target) return
        reset()
        target = next
        if (next == Target.DESKTOP || next == Target.GAME) handler.post(frame)
    }

    private fun reset() {
        handler.removeCallbacks(frame)
        if (dragging) desktopButton(false)
        if (target == Target.GAME) {
            gameAxes(GamepadAxes())
            gameButtons(0)
        }
        dragging = false
        axes = GamepadAxes()
        buttons = 0
        hat = 0
        lastButtons = 0
        digitalLeftTrigger = false
        digitalRightTrigger = false
        wheel = 0f
        escapeSince = 0
    }

    override fun onInputDeviceAdded(deviceId: Int) = refreshDevice()
    override fun onInputDeviceChanged(deviceId: Int) = refreshDevice()
    override fun onInputDeviceRemoved(deviceId: Int) = refreshDevice()

    private fun refreshDevice() {
        val current = device
        val next = current?.let { manager.getInputDevice(it.id) }?.takeIf(::isController)
            ?: manager.inputDeviceIds.asSequence().mapNotNull(manager::getInputDevice).firstOrNull(::isController)
        device = next
        if (current?.id != next?.id || current?.descriptor != next?.descriptor) {
            val previousTarget = target
            setTarget(Target.NONE)
            if (previousTarget == Target.GAME || previousTarget == Target.TRANSITION) stopGame()
            onDevice(next?.name)
        } else if (current?.name != next?.name) {
            onDevice(next?.name)
        }
    }

    private fun isController(device: InputDevice): Boolean {
        if (device.isVirtual) return false
        if (device.supportsSource(InputDevice.SOURCE_GAMEPAD)) return true
        // Some phones expose an internal joystick source for built-in input features.
        return device.supportsSource(InputDevice.SOURCE_JOYSTICK) &&
            (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q || device.isExternal)
    }

    fun key(event: KeyEvent): Boolean {
        if (target == Target.NONE || event.deviceId != device?.id) return false
        val down = event.action == KeyEvent.ACTION_DOWN
        if (!down && event.action != KeyEvent.ACTION_UP) return false
        val code = event.keyCode
        val mask = buttonMask(code)
        val trigger = code == KeyEvent.KEYCODE_BUTTON_L2 || code == KeyEvent.KEYCODE_BUTTON_R2
        if (mask == 0 && !trigger) return false
        when (target) {
            Target.KEYBOARD -> {
                if (mask in 1..8) {
                    val wasDown = (buttons or hat) and mask != 0
                    buttons = if (down) buttons or mask else buttons and mask.inv()
                    if (down && (!wasDown || event.repeatCount > 0)) onKeyboard(code)
                } else if (down && (event.repeatCount == 0 || code == KeyEvent.KEYCODE_BUTTON_X)) {
                    onKeyboard(code)
                }
            }
            Target.DESKTOP -> when {
                code == KeyEvent.KEYCODE_BUTTON_A && down != dragging && event.repeatCount == 0 -> desktopButton(down)
                down && event.repeatCount == 0 -> when (code) {
                    KeyEvent.KEYCODE_BUTTON_B -> send(message("click").put("button", "right"), false)
                    KeyEvent.KEYCODE_BUTTON_SELECT -> openKeyboard()
                    KeyEvent.KEYCODE_BUTTON_START -> startGame()
                }
            }
            Target.GAME -> {
                if (trigger) {
                    if (code == KeyEvent.KEYCODE_BUTTON_L2) digitalLeftTrigger = down else digitalRightTrigger = down
                    sendAxes()
                } else {
                    buttons = if (down) buttons or mask else buttons and mask.inv()
                    sendButtons()
                }
            }
            else -> Unit
        }
        return true
    }

    fun motion(event: MotionEvent): Boolean {
        val current = device ?: return false
        if (target == Target.NONE || event.deviceId != current.id ||
            !event.isFromSource(InputDevice.SOURCE_JOYSTICK) || event.action != MotionEvent.ACTION_MOVE
        ) return false

        val hx = event.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hy = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
        val nextHat = (if (hx < -.5f) 4 else if (hx > .5f) 8 else 0) or
            (if (hy < -.5f) 1 else if (hy > .5f) 2 else 0)
        val changed = nextHat and (hat or buttons).inv()
        hat = nextHat
        if (target == Target.KEYBOARD) {
            if (changed and 1 != 0) onKeyboard(KeyEvent.KEYCODE_DPAD_UP)
            if (changed and 2 != 0) onKeyboard(KeyEvent.KEYCODE_DPAD_DOWN)
            if (changed and 4 != 0) onKeyboard(KeyEvent.KEYCODE_DPAD_LEFT)
            if (changed and 8 != 0) onKeyboard(KeyEvent.KEYCODE_DPAD_RIGHT)
        }
        if (target == Target.DESKTOP || target == Target.GAME) {
            axes = GamepadAxes(
                lx = stick(event, current, MotionEvent.AXIS_X),
                ly = -stick(event, current, MotionEvent.AXIS_Y),
                rx = stick(event, current, MotionEvent.AXIS_Z, MotionEvent.AXIS_RX),
                ry = -stick(event, current, MotionEvent.AXIS_RZ, MotionEvent.AXIS_RY),
                lt = trigger(event, current, MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_BRAKE),
                rt = trigger(event, current, MotionEvent.AXIS_RTRIGGER, MotionEvent.AXIS_GAS),
            )
        }
        if (target == Target.GAME) {
            sendAxes()
            sendButtons()
        }
        return true
    }

    private fun sendButtons() {
        val combined = buttons or hat
        if (combined == lastButtons) return
        lastButtons = combined
        gameButtons(combined)
        if (combined and 0x30 == 0x30) {
            if (escapeSince == 0L) escapeSince = SystemClock.uptimeMillis()
        } else {
            escapeSince = 0
        }
    }

    private fun sendAxes() = gameAxes(axes.copy(
        lt = if (digitalLeftTrigger && axes.lt == 0) 255 else axes.lt,
        rt = if (digitalRightTrigger && axes.rt == 0) 255 else axes.rt,
    ))

    private fun desktopButton(down: Boolean) {
        dragging = down
        send(message("drag").put("down", down), false)
    }

    private fun moveDesktop() {
        val x = pointerStep(axes.lx)
        val y = -pointerStep(axes.ly)
        if (x != 0 || y != 0) send(message("move").put("x", x).put("y", y), true)
        if (axes.ry == 0) {
            wheel = 0f
        } else {
            wheel += axes.ry / 32767f * .22f
            val detents = wheel.toInt()
            if (detents != 0) {
                wheel -= detents
                send(message("wheel").put("delta", detents * 120), true)
            }
        }
    }

    private fun pointerStep(value: Int): Int {
        val unit = value / 32767f
        return (unit * (2f + 24f * abs(unit))).roundToInt()
    }

    private fun stick(event: MotionEvent, device: InputDevice, primary: Int, fallback: Int = primary): Int {
        val range = range(event, device, primary, fallback) ?: return 0
        val raw = event.getAxisValue(range.axis)
        val limit = if (raw < 0) -range.min else range.max
        if (limit <= 0f) return 0
        val unit = (raw / limit).coerceIn(-1f, 1f)
        val deadzone = (range.flat / limit).coerceIn(.12f, .4f)
        if (abs(unit) <= deadzone) return 0
        val centered = if (unit < 0) unit + deadzone else unit - deadzone
        return (32767 * centered / (1 - deadzone)).roundToInt()
    }

    private fun trigger(event: MotionEvent, device: InputDevice, primary: Int, fallback: Int): Int {
        val range = range(event, device, primary, fallback) ?: return 0
        if (range.range <= 0f) return 0
        val displacement = event.getAxisValue(range.axis) - range.min
        if (displacement <= range.flat) return 0
        return (255 * (displacement / range.range).coerceIn(0f, 1f)).roundToInt()
    }

    private fun range(event: MotionEvent, device: InputDevice, primary: Int, fallback: Int) =
        device.getMotionRange(primary, event.source) ?: device.getMotionRange(fallback, event.source)
            ?: device.getMotionRange(primary) ?: device.getMotionRange(fallback)

    private fun message(type: String) = JSONObject().put("type", type)

    private fun buttonMask(code: Int) = when (code) {
        KeyEvent.KEYCODE_DPAD_UP -> 0x0001
        KeyEvent.KEYCODE_DPAD_DOWN -> 0x0002
        KeyEvent.KEYCODE_DPAD_LEFT -> 0x0004
        KeyEvent.KEYCODE_DPAD_RIGHT -> 0x0008
        KeyEvent.KEYCODE_BUTTON_START -> 0x0010
        KeyEvent.KEYCODE_BUTTON_SELECT -> 0x0020
        KeyEvent.KEYCODE_BUTTON_THUMBL -> 0x0040
        KeyEvent.KEYCODE_BUTTON_THUMBR -> 0x0080
        KeyEvent.KEYCODE_BUTTON_L1 -> 0x0100
        KeyEvent.KEYCODE_BUTTON_R1 -> 0x0200
        KeyEvent.KEYCODE_BUTTON_A -> 0x1000
        KeyEvent.KEYCODE_BUTTON_B -> 0x2000
        KeyEvent.KEYCODE_BUTTON_X -> 0x4000
        KeyEvent.KEYCODE_BUTTON_Y -> 0x8000
        else -> 0
    }
}
