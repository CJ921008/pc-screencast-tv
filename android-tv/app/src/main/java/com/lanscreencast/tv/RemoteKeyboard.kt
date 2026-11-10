package com.lanscreencast.tv

import android.app.AlertDialog
import android.content.Context
import android.text.InputFilter
import android.text.InputType
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.ToggleButton
import org.json.JSONObject

internal fun textInputDialog(context: Context, send: (JSONObject) -> Unit): AlertDialog {
    val field = EditText(context).apply {
        hint = "输入要发送到电脑的文字"
        inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
        filters = arrayOf(InputFilter.LengthFilter(256))
        minLines = 2
        maxLines = 4
    }
    val dialog = AlertDialog.Builder(context)
        .setTitle("输入文字")
        .setView(FrameLayout(context).apply {
            val inset = (20 * resources.displayMetrics.density).toInt()
            setPadding(inset, 0, inset, 0)
            addView(field)
        })
        .setNegativeButton("关闭", null)
        .setPositiveButton("发送", null)
        .create()
    dialog.setOnShowListener {
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
            val text = field.text.toString()
            if (text.isNotEmpty()) {
                send(JSONObject().put("type", "text").put("text", text))
                field.text.clear()
            }
        }
    }
    field.requestFocus()
    dialog.window?.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE)
    return dialog
}

internal fun shortcutDialog(context: Context, send: (JSONObject) -> Unit): AlertDialog {
    val content = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        val inset = (12 * resources.displayMetrics.density).toInt()
        setPadding(inset, inset, inset, inset)
    }
    fun row() = LinearLayout(context).also { content.addView(it) }
    fun cell() = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)

    val modifierRow = row()
    val modifiers = listOf("Ctrl", "Shift", "Alt", "Win").map { label ->
        ToggleButton(context).apply {
            textOn = label
            textOff = label
            text = label
            contentDescription = label
            minWidth = 0
            minimumWidth = 0
            modifierRow.addView(this, cell())
        }
    }
    fun keyRow(keys: List<Pair<String, Int>>) {
        val container = row()
        keys.forEach { (label, code) ->
            container.addView(Button(context).apply {
                text = label
                isAllCaps = false
                minWidth = 0
                minimumWidth = 0
                setPadding(0, 0, 0, 0)
                setOnClickListener {
                    val mask = modifiers.foldIndexed(0) { index, flags, key ->
                        flags or if (key.isChecked) (1 shl index) else 0
                    }
                    send(JSONObject().put("type", "key").put("code", code).put("modifiers", mask))
                    modifiers.forEach { it.isChecked = false }
                }
            }, cell())
        }
    }
    keyRow(listOf("Esc" to 0x1b, "Tab" to 0x09, "回车" to 0x0d, "退格" to 0x08, "删除" to 0x2e))
    keyRow(listOf("←" to 0x25, "↑" to 0x26, "↓" to 0x28, "→" to 0x27, "空格" to 0x20))
    keyRow(listOf("Home" to 0x24, "End" to 0x23, "PgUp" to 0x21, "PgDn" to 0x22, "Win" to 0x5b))
    (1..12).chunked(6).forEach { numbers -> keyRow(numbers.map { "F$it" to (0x6f + it) }) }
    listOf("123456", "7890", "ABCDEF", "GHIJKL", "MNOPQR", "STUVWX", "YZ").forEach { keys ->
        keyRow(keys.map { it.toString() to it.code })
    }
    return AlertDialog.Builder(context)
        .setTitle("快捷键 · 先选修饰键，再点按键")
        .setView(ScrollView(context).apply { addView(content) })
        .setPositiveButton("关闭", null)
        .create()
}
