package com.lanscreencast.tv

import android.content.Context
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import org.json.JSONObject
import kotlin.math.abs
import kotlin.math.roundToInt

internal data class ControlView(
    val left: Float,
    val top: Float,
    val right: Float,
    val bottom: Float,
    val cursorX: Float?,
    val cursorY: Float?,
    val blocked: Boolean,
) {
    companion object {
        fun parse(message: String): ControlView? = runCatching {
            val json = JSONObject(message)
            require(json.getString("type") == "view")
            val rect = json.getJSONArray("rect")
            val l = rect.getInt(0)
            val t = rect.getInt(1)
            val r = rect.getInt(2)
            val b = rect.getInt(3)
            require(l in 0 until r && t in 0 until b && r <= 10000 && b <= 10000)
            val cursor = json.optJSONArray("cursor")
            fun coordinate(index: Int) = cursor?.getInt(index)?.also { require(it in 0..10000) }?.div(10000f)
            ControlView(l / 10000f, t / 10000f, r / 10000f, b / 10000f,
                coordinate(0), coordinate(1), json.getBoolean("blocked"))
        }.getOrNull()
    }
}

internal class RemoteTouchView(
    context: Context,
    private val send: (JSONObject, Boolean) -> Unit,
    private val onCancelDrag: () -> Unit,
) : View(context) {
    private val slop = ViewConfiguration.get(context).scaledTouchSlop
    private val density = resources.displayMetrics.density
    private var controlView: ControlView? = null
    private var direct = false
    private var dragEnabled = false
    private var directDrag = false
    private var moved = false
    private var longPressed = false
    private var fingers = 0
    private var downAt = 0L
    private var lastX = 0f
    private var lastY = 0f
    private var dx = 0f
    private var dy = 0f
    private var wheel = 0f
    private var point: Pair<Int, Int>? = null

    private val frame = Runnable { flushMotion() }
    private val longPress = Runnable {
        if (fingers == 1 && !moved && !dragEnabled) {
            position(lastX, lastY, clamp = false)?.let {
                longPressed = true
                click("right", it)
            }
        }
    }

    fun update(view: ControlView, direct: Boolean, drag: Boolean) {
        controlView = view
        if (this.direct != direct || dragEnabled != drag) {
            if (directDrag || (dragEnabled && !this.direct)) button(false)
            directDrag = false
            removeCallbacks(longPress)
            if (drag && !direct) button(true)
            dragEnabled = drag
            this.direct = direct
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                parent.requestDisallowInterceptTouchEvent(true)
                fingers = 1
                moved = false
                longPressed = false
                downAt = event.eventTime
                lastX = event.x
                lastY = event.y
                dx = 0f
                dy = 0f
                wheel = 0f
                if (direct && dragEnabled) {
                    position(lastX, lastY, clamp = false)?.let {
                        button(true, it)
                        directDrag = true
                    }
                } else if (direct && !dragEnabled) {
                    postDelayed(longPress, ViewConfiguration.getLongPressTimeout().toLong())
                }
            }
            MotionEvent.ACTION_POINTER_DOWN -> {
                removeCallbacks(longPress)
                if (directDrag) {
                    flushMotion()
                    button(false)
                    directDrag = false
                    onCancelDrag()
                    moved = true
                }
                fingers = maxOf(fingers, event.pointerCount)
                if (event.pointerCount == 2) lastY = (event.getY(0) + event.getY(1)) / 2
            }
            MotionEvent.ACTION_MOVE -> {
                if (fingers == 1) {
                    val stepX = event.x - lastX
                    val stepY = event.y - lastY
                    if (moved || abs(stepX) > slop || abs(stepY) > slop) {
                        moved = true
                        removeCallbacks(longPress)
                        if (direct) {
                            if (directDrag) point = position(event.x, event.y, clamp = true)
                        } else {
                            dx += stepX * 1.8f / density
                            dy += stepY * 1.8f / density
                        }
                        lastX = event.x
                        lastY = event.y
                        scheduleMotion()
                    }
                } else if (fingers == 2 && event.pointerCount == 2 && !dragEnabled) {
                    val y = (event.getY(0) + event.getY(1)) / 2
                    if (moved || abs(y - lastY) > slop) {
                        moved = true
                        wheel += (y - lastY) / density
                        lastY = y
                        scheduleMotion()
                    }
                }
            }
            MotionEvent.ACTION_UP -> {
                removeCallbacks(longPress)
                flushMotion()
                if (directDrag) {
                    button(false)
                    directDrag = false
                    onCancelDrag()
                } else if (!moved && !longPressed && !dragEnabled &&
                    event.eventTime - downAt < ViewConfiguration.getLongPressTimeout()) {
                    val location = if (direct) position(event.x, event.y, clamp = false) else null
                    if (fingers <= 2 && (!direct || location != null)) {
                        performClick()
                        click(if (fingers == 2) "right" else "left", location)
                    }
                }
                fingers = 0
            }
            MotionEvent.ACTION_CANCEL -> cancelGesture()
        }
        return true
    }

    override fun performClick(): Boolean = super.performClick()

    private fun scheduleMotion() {
        removeCallbacks(frame)
        postOnAnimation(frame)
    }

    private fun flushMotion() {
        removeCallbacks(frame)
        val x = dx.toInt().coerceIn(-1000, 1000)
        val y = dy.toInt().coerceIn(-1000, 1000)
        dx -= dx.toInt()
        dy -= dy.toInt()
        if (x != 0 || y != 0) send(message("move").put("x", x).put("y", y), true)
        point?.let {
            send(message("point").put("x", it.first).put("y", it.second), true)
            point = null
        }
        val detents = (wheel / 24f).toInt().coerceIn(-8, 8)
        if (detents != 0) {
            wheel -= detents * 24f
            send(message("wheel").put("delta", detents * 120), true)
        }
    }

    private fun position(x: Float, y: Float, clamp: Boolean): Pair<Int, Int>? {
        val view = controlView ?: return null
        if (width == 0 || height == 0) return null
        val u = (x / width - view.left) / (view.right - view.left)
        val v = (y / height - view.top) / (view.bottom - view.top)
        if (!clamp && (u !in 0f..1f || v !in 0f..1f)) return null
        return (u.coerceIn(0f, 1f) * 10000).roundToInt() to (v.coerceIn(0f, 1f) * 10000).roundToInt()
    }

    private fun sendAt(command: JSONObject, location: Pair<Int, Int>?) {
        location?.let { command.put("x", it.first).put("y", it.second) }
        send(command, false)
    }

    private fun button(down: Boolean, location: Pair<Int, Int>? = null) =
        sendAt(message("drag").put("down", down), location)
    private fun click(button: String, location: Pair<Int, Int>?) =
        sendAt(message("click").put("button", button), location)
    private fun message(type: String) = JSONObject().put("type", type)

    private fun cancelGesture() {
        removeCallbacks(longPress)
        removeCallbacks(frame)
        dx = 0f
        dy = 0f
        wheel = 0f
        point = null
        fingers = 0
        if (dragEnabled || directDrag) {
            button(false)
            directDrag = false
            dragEnabled = false
            onCancelDrag()
        }
    }

    override fun onDetachedFromWindow() {
        cancelGesture()
        super.onDetachedFromWindow()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        if (oldw > 0 && oldh > 0) cancelGesture()
    }
}
