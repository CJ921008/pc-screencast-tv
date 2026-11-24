package com.lanscreencast.tv

import android.content.pm.PackageManager
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.tv.material3.MaterialTheme
import androidx.tv.material3.Text

@Composable
internal fun ControlledVideo(
    receiver: WebRtcReceiver,
    connecting: Boolean,
    control: ControlView?,
    direct: Boolean,
    dragging: Boolean,
    hardware: RemoteHardwareInput,
    menuOpen: Boolean,
    onMenuChange: (Boolean) -> Unit,
    onMode: () -> Unit,
    onDrag: (Boolean) -> Unit,
    onText: () -> Unit,
    onShortcuts: () -> Unit,
    onDisconnect: (() -> Unit)?,
    audioState: AudioPlaybackState?,
    onAudioToggle: () -> Unit,
) {
    val context = LocalContext.current
    val tv = remember(context) { context.packageManager.hasSystemFeature(PackageManager.FEATURE_LEANBACK) }
    val openFocus = remember { FocusRequester() }
    val closeFocus = remember { FocusRequester() }
    val hasActions = control != null || audioState != null || onDisconnect != null
    val showMenu = hasActions && menuOpen

    LaunchedEffect(hasActions, showMenu) {
        if (!hasActions) onMenuChange(false)
        else if (tv) (if (showMenu) closeFocus else openFocus).requestFocus()
    }
    BackHandler(enabled = showMenu) { onMenuChange(false) }

    Box(Modifier.fillMaxSize().background(Color.Black), contentAlignment = Alignment.Center) {
        // The encoded frame is always 16:9. Keep touch, cursor and video in the same rectangle.
        Box(Modifier.aspectRatio(16f / 9f).clipToBounds()) {
            AndroidView(
                factory = { receiver.createRenderer() },
                onRelease = { receiver.releaseRenderer(it) },
                modifier = Modifier.matchParentSize(),
            )
            if (control != null) {
                AndroidView(
                    factory = { RemoteTouchView(it, receiver::sendControl, hardware) { onDrag(false) } },
                    update = { it.update(control, direct, dragging) },
                    modifier = Modifier.matchParentSize(),
                )
                RemoteCursor(control, Modifier.matchParentSize())
            }
        }

        if (connecting) Text("正在建立视频连接…", color = CastMuted)

        if (hasActions) {
            if (showMenu) {
                Box(Modifier.matchParentSize().background(Color(0x4407100D))
                    .pointerInput(Unit) { detectTapGestures { onMenuChange(false) } })
            }

            Box(
                Modifier.align(Alignment.TopEnd).matchParentSize()
                    .windowInsetsPadding(WindowInsets.displayCutout).padding(12.dp),
                contentAlignment = Alignment.TopEnd,
            ) {
                if (showMenu) {
                    val shape = RoundedCornerShape(16.dp)
                    Column(
                        Modifier.widthIn(max = 274.dp).fillMaxWidth().background(CastPanel, shape)
                            .border(1.dp, CastBorder, shape)
                            .pointerInput(Unit) { detectTapGestures(onTap = {}) }
                            .verticalScroll(rememberScrollState()).padding(12.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically) {
                            Text("投屏控制", color = CastText, style = MaterialTheme.typography.titleMedium)
                            MenuIcon("×", "收起投屏控制", Modifier.focusRequester(closeFocus)) { onMenuChange(false) }
                        }
                        if (control != null) {
                            Text(
                                when {
                                    control.blocked -> "Windows 未接受上一次输入"
                                    direct -> "点按左键 · 长按右键 · 双指滚动"
                                    else -> "单指移动和点击 · 双指右键和滚动"
                                },
                                color = if (control.blocked) CastWarning else CastMuted,
                                style = MaterialTheme.typography.bodySmall,
                            )
                            if (control.hardwareInput) Text("外接键盘和鼠标可直接控制 Windows",
                                color = CastMuted, style = MaterialTheme.typography.bodySmall)
                            CastAction(if (direct) "当前为直接触摸" else "当前为触控板", onMode, Modifier.fillMaxWidth())
                            CastAction(if (dragging) "松开" else "拖拽", {
                                onDrag(!dragging)
                                onMenuChange(false)
                            }, Modifier.fillMaxWidth())
                            CastAction("输入文字", { onMenuChange(false); onText() }, Modifier.fillMaxWidth())
                            CastAction("快捷键", { onMenuChange(false); onShortcuts() }, Modifier.fillMaxWidth())
                        }
                        audioState?.let {
                            val label = when {
                                it.muted -> "开启声音"
                                it.interrupted -> "恢复声音"
                                else -> "静音"
                            }
                            CastAction(label, onAudioToggle, Modifier.fillMaxWidth())
                        }
                        onDisconnect?.let { CastAction("断开远程连接", it, Modifier.fillMaxWidth()) }
                    }
                } else {
                    MenuIcon("⋯", "显示投屏控制", Modifier.focusRequester(openFocus)) { onMenuChange(true) }
                }
            }
        }
    }
}

@Composable
private fun MenuIcon(symbol: String, label: String, modifier: Modifier, onClick: () -> Unit) {
    CastButton(modifier.size(48.dp).semantics { contentDescription = label }, onClick, shape = CircleShape) { color ->
        Text(symbol, color = color, style = MaterialTheme.typography.titleLarge)
    }
}

@Composable
private fun RemoteCursor(control: ControlView, modifier: Modifier) {
    val cursorX = control.cursorX ?: return
    val cursorY = control.cursorY ?: return
    Canvas(modifier) {
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
