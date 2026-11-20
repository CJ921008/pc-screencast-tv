package com.lanscreencast.tv

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.tv.material3.Button
import androidx.tv.material3.ButtonDefaults
import androidx.tv.material3.MaterialTheme
import androidx.tv.material3.Text
import androidx.tv.material3.darkColorScheme

internal val CastBackground = Color(0xFF101614)
internal val CastPanel = Color(0xFF18211E)
internal val CastBorder = Color(0xFF35443D)
internal val CastText = Color(0xFFF1F6F2)
internal val CastMuted = Color(0xFFB3C5BB)
internal val CastMint = Color(0xFF9CEBBD)
internal val CastWarning = Color(0xFFFFC596)
private val CastInk = Color(0xFF102318)

@Composable
internal fun CastTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = darkColorScheme(
            background = CastBackground,
            onBackground = CastText,
            surface = CastPanel,
            onSurface = CastText,
            surfaceVariant = CastBorder,
            onSurfaceVariant = CastMuted,
            primary = CastMint,
            onPrimary = CastInk,
            secondaryContainer = Color(0xFF23382D),
            onSecondaryContainer = CastMint,
        ),
        content = content,
    )
}

@Composable
internal fun CastAction(
    label: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    primary: Boolean = false,
) {
    Button(
        onClick = onClick,
        modifier = modifier.heightIn(min = 48.dp),
        colors = ButtonDefaults.colors(
            containerColor = if (primary) CastMint else Color(0xFF26342D),
            contentColor = if (primary) CastInk else CastText,
            focusedContainerColor = Color(0xFFDAF9E6),
            focusedContentColor = CastInk,
            pressedContainerColor = CastMint,
            pressedContentColor = CastInk,
        ),
    ) { Text(label) }
}

@Composable
internal fun ReceiverHome(
    status: String,
    localAddress: String?,
    remoteActive: Boolean,
    paired: Boolean,
    onConnect: () -> Unit,
    onPair: () -> Unit,
    onCancel: () -> Unit,
) {
    BoxWithConstraints(
        Modifier.fillMaxSize().background(CastBackground).safeDrawingPadding(),
        contentAlignment = Alignment.TopCenter,
    ) {
        val wide = maxWidth >= 680.dp
        val intro: @Composable () -> Unit = {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text("LAN ScreenCast", color = CastMint, fontWeight = FontWeight.SemiBold)
                Text("投屏接收器", color = CastText, style = MaterialTheme.typography.headlineLarge)
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Box(Modifier.size(8.dp).background(CastMint, CircleShape))
                    Text(status, color = CastMuted)
                }
            }
        }
        val options: @Composable () -> Unit = {
            Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
                ReceiverPanel {
                    Text("家里电脑", style = MaterialTheme.typography.titleLarge, color = CastText)
                    Text("通过公网 IPv6 接收桌面画面和声音。", color = CastMuted)
                    CastAction(
                        when {
                            remoteActive -> "取消远程连接"
                            paired -> "连接已配对电脑"
                            else -> "连接家里电脑"
                        },
                        when {
                            remoteActive -> onCancel
                            paired -> onConnect
                            else -> onPair
                        },
                        Modifier.fillMaxWidth(),
                        primary = !remoteActive,
                    )
                    if (paired && !remoteActive) {
                        CastAction("重新配对或忘记电脑", onPair, Modifier.fillMaxWidth())
                    }
                }
                ReceiverPanel {
                    Text("局域网接收", style = MaterialTheme.typography.titleLarge, color = CastText)
                    Text(localAddress ?: "未连接局域网", color = CastMint,
                        fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.titleMedium)
                    if (localAddress != null) Text("在 Windows 端输入此地址，开始投屏。", color = CastMuted)
                }
            }
        }
        if (wide) {
            Row(
                Modifier.widthIn(max = 1000.dp).fillMaxWidth().verticalScroll(rememberScrollState())
                    .padding(horizontal = 36.dp, vertical = 28.dp),
                horizontalArrangement = Arrangement.spacedBy(28.dp),
            ) {
                Box(Modifier.weight(1f)) { intro() }
                Box(Modifier.weight(1.35f)) { options() }
            }
        } else {
            Column(
                Modifier.widthIn(max = 480.dp).fillMaxWidth().verticalScroll(rememberScrollState())
                    .padding(horizontal = 24.dp, vertical = 24.dp),
                verticalArrangement = Arrangement.spacedBy(22.dp),
            ) { intro(); options() }
        }
    }
}

@Composable
private fun ReceiverPanel(content: @Composable () -> Unit) {
    val shape = RoundedCornerShape(16.dp)
    Column(
        Modifier.fillMaxWidth().background(CastPanel, shape).border(1.dp, CastBorder, shape).padding(18.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) { content() }
}
