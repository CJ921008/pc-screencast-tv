package com.lanscreencast.tv

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.sizeIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.tv.material3.MaterialTheme
import androidx.tv.material3.Text

@Composable
internal fun ControlledVideo(
    receiver: WebRtcReceiver,
    control: ControlView?,
    direct: Boolean,
    dragging: Boolean,
    onMode: () -> Unit,
    onDrag: (Boolean) -> Unit,
    onText: () -> Unit,
    onShortcuts: () -> Unit,
) {
    Column(Modifier.fillMaxSize().background(Color.Black)) {
        Box(Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) {
            Box(Modifier.aspectRatio(16f / 9f).clipToBounds()) {
                AndroidView(factory = { receiver.createRenderer() },
                    onRelease = { receiver.releaseRenderer(it) }, modifier = Modifier.matchParentSize())
                if (control != null) {
                    AndroidView(
                        factory = { RemoteTouchView(it, receiver::sendControl) { onDrag(false) } },
                        update = { it.update(control, direct, dragging) },
                        modifier = Modifier.matchParentSize(),
                    )
                    val cursorX = control.cursorX
                    val cursorY = control.cursorY
                    if (cursorX != null && cursorY != null) {
                        Canvas(Modifier.matchParentSize()) {
                            val x = (control.left + (control.right - control.left) * cursorX) * size.width
                            val y = (control.top + (control.bottom - control.top) * cursorY) * size.height
                            val length = 14.dp.toPx()
                            val arrow = Path().apply {
                                moveTo(x, y)
                                lineTo(x + length, y + length * .7f)
                                lineTo(x + length * .43f, y + length * .79f)
                                lineTo(x + length * .26f, y + length * 1.14f)
                                close()
                            }
                            drawPath(arrow, Color.White)
                            drawPath(arrow, Color.Black, style = Stroke(1.dp.toPx()))
                        }
                    }
                }
            }
        }
        if (control != null) {
            Text(
                if (control.blocked) "Windows 未接受上一次输入"
                else if (direct) "点按左键 · 长按右键 · 双指滚动"
                else "单指移动和点击 · 双指右键和滚动",
                modifier = Modifier.padding(horizontal = 12.dp, vertical = 2.dp),
                color = Color.White,
            )
            Row(
                Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = 8.dp, vertical = 4.dp),
                horizontalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                ControlButton(if (direct) "直接触摸" else "触控板", onMode)
                ControlButton(if (dragging) "松开" else "拖拽") { onDrag(!dragging) }
                ControlButton("输入文字", onText)
                ControlButton("快捷键", onShortcuts)
            }
        }
    }
}

@Composable
private fun ControlButton(label: String, onClick: () -> Unit) {
    val colors = MaterialTheme.colorScheme
    Box(
        Modifier.clip(RoundedCornerShape(24.dp)).background(colors.secondaryContainer)
            .clickable(role = Role.Button, onClick = onClick)
            .sizeIn(minWidth = 48.dp, minHeight = 48.dp).padding(horizontal = 14.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(label, color = colors.onSecondaryContainer)
    }
}
