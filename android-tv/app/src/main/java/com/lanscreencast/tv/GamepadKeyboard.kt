package com.lanscreencast.tv

import android.view.KeyEvent
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.tv.material3.MaterialTheme
import androidx.tv.material3.Text
import org.json.JSONObject

internal data class GamepadKeyboardState(
    val row: Int = 0,
    val column: Int = 0,
    val upper: Boolean = false,
    val symbols: Boolean = false,
) {
    val rows: List<List<String>>
        get() {
            val keys = if (symbols) {
                listOf("1234567890", "!@#\$%^&*()", "-_=+[]{}\\|", ";\"':,.<>/?")
            } else {
                listOf("1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm,.?")
                    .map { if (upper) it.uppercase() else it }
            }
            return keys.map { line -> line.map(Char::toString) } +
                listOf(listOf(if (upper) "小写" else "大写", if (symbols) "字母" else "符号", "空格", "退格", "回车", "关闭"))
        }

    fun move(code: Int): GamepadKeyboardState {
        val nextRow = (row + when (code) {
            KeyEvent.KEYCODE_DPAD_UP -> -1
            KeyEvent.KEYCODE_DPAD_DOWN -> 1
            else -> 0
        }).coerceIn(rows.indices)
        val nextColumn = (column + when (code) {
            KeyEvent.KEYCODE_DPAD_LEFT -> -1
            KeyEvent.KEYCODE_DPAD_RIGHT -> 1
            else -> 0
        }).coerceIn(rows[nextRow].indices)
        return copy(row = nextRow, column = nextColumn)
    }

    val selected: String get() = rows[row][column]
}

internal fun gamepadKeyboardMessage(label: String): JSONObject = when (label) {
    "退格", "回车" -> JSONObject().put("type", "key").put("code", if (label == "退格") 8 else 13)
        .put("modifiers", 0)
    else -> JSONObject().put("type", "text").put("text", if (label == "空格") " " else label)
}

@Composable
internal fun GamepadKeyboard(state: GamepadKeyboardState, onKey: (String) -> Unit, onClose: () -> Unit) {
    Box(
        Modifier.fillMaxSize().background(Color(0x55000000))
            .pointerInput(onClose) { detectTapGestures { onClose() } }
            .windowInsetsPadding(WindowInsets.displayCutout).padding(8.dp),
        contentAlignment = Alignment.BottomCenter,
    ) {
        val shape = RoundedCornerShape(16.dp)
        Column(
            Modifier.widthIn(max = 740.dp).fillMaxWidth().background(CastPanel, shape).border(1.dp, CastBorder, shape)
                .pointerInput(Unit) { detectTapGestures {} }
                .verticalScroll(rememberScrollState()).padding(8.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            Text("方向键选字 · A 输入 · X 退格 · B 关闭", color = CastMuted,
                style = MaterialTheme.typography.bodySmall)
            state.rows.forEachIndexed { row, keys ->
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    keys.forEachIndexed { column, label ->
                        CastButton(
                            Modifier.weight(1f),
                            { onKey(label) },
                            primary = row == state.row && column == state.column,
                        ) { color ->
                            Text(label, color = color, maxLines = 1, overflow = TextOverflow.Clip,
                                style = MaterialTheme.typography.bodySmall)
                        }
                    }
                }
            }
        }
    }
}
