package com.lanscreencast.tv.logging

import android.content.Context
import java.io.File
import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter

object FileLogger {
    private val formatter = DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'")
        .withZone(ZoneOffset.UTC)

    @Synchronized
    fun log(context: Context, level: String, module: String, event: String, message: String) {
        val directory = File(context.filesDir, "logs")
        if (!directory.exists() && !directory.mkdirs()) return
        val line = listOf(formatter.format(Instant.now()), level, module, event, message)
            .joinToString("\t") { it.replace('\n', ' ').replace('\r', ' ').replace('\t', ' ') }
        runCatching { File(directory, "lanscreencast.log").appendText("$line\n") }
            .onFailure { android.util.Log.e("LANScreenCast", "Failed to write log", it) }
    }
}

