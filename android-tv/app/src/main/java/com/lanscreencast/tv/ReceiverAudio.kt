package com.lanscreencast.tv

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import org.webrtc.AudioTrack
import org.webrtc.PeerConnection

internal data class AudioPlaybackState(val muted: Boolean, val interrupted: Boolean)

// The receiver worker owns playback. Focus callbacks are marshalled to the same worker.
internal class ReceiverAudio(
    context: Context,
    private val dispatch: (() -> Unit) -> Unit,
    private val onState: (AudioPlaybackState?) -> Unit,
) {
    val attributes: AudioAttributes = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_MEDIA)
        .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
        .build()
    private val manager = requireNotNull(context.getSystemService(AudioManager::class.java))
    private val mainHandler = Handler(Looper.getMainLooper())
    private var peer: PeerConnection? = null
    private var track: AudioTrack? = null
    private var foreground = false
    private var muted = false
    private var focusGranted = false
    private var focusListener: AudioManager.OnAudioFocusChangeListener? = null
    private var focusRequest: AudioFocusRequest? = null

    fun attach(connection: PeerConnection, incoming: AudioTrack) {
        incoming.setEnabled(false)
        track?.setEnabled(false)
        peer = connection
        track = incoming
        updateFocus()
    }

    fun setForeground(active: Boolean) {
        foreground = active
        updateFocus()
    }

    fun setMuted(value: Boolean) {
        muted = value
        if (!value && !focusGranted) abandonFocus()
        updateFocus()
    }

    private fun updateFocus() {
        if (track == null || !foreground || muted) abandonFocus()
        else if (focusListener == null) requestFocus()
        applyPlayback()
    }

    private fun requestFocus() {
        val listener = object : AudioManager.OnAudioFocusChangeListener {
            override fun onAudioFocusChange(change: Int) = dispatch {
                if (focusListener !== this) return@dispatch
                focusGranted = change == AudioManager.AUDIOFOCUS_GAIN
                if (change == AudioManager.AUDIOFOCUS_LOSS) abandonFocus()
                applyPlayback()
            }
        }
        focusListener = listener
        val result = runCatching {
            if (Build.VERSION.SDK_INT >= 26) {
                val request = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                    .setAudioAttributes(attributes)
                    .setAcceptsDelayedFocusGain(true)
                    .setWillPauseWhenDucked(true)
                    .setOnAudioFocusChangeListener(listener, mainHandler)
                    .build()
                focusRequest = request
                manager.requestAudioFocus(request)
            } else {
                @Suppress("DEPRECATION")
                manager.requestAudioFocus(listener, AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN)
            }
        }.getOrDefault(AudioManager.AUDIOFOCUS_REQUEST_FAILED)
        focusGranted = result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
        if (result == AudioManager.AUDIOFOCUS_REQUEST_FAILED) abandonFocus()
    }

    private fun abandonFocus() {
        val listener = focusListener
        val request = focusRequest
        focusListener = null
        focusRequest = null
        focusGranted = false
        if (listener == null) return
        runCatching {
            if (Build.VERSION.SDK_INT >= 26 && request != null) manager.abandonAudioFocusRequest(request)
            else {
                @Suppress("DEPRECATION")
                manager.abandonAudioFocus(listener)
            }
        }
    }

    private fun applyPlayback() {
        val enabled = track != null && foreground && !muted && focusGranted
        track?.setEnabled(enabled)
        peer?.setAudioPlayout(enabled)
        onState(track?.let { AudioPlaybackState(muted, foreground && !muted && !focusGranted) })
    }

    fun clear() {
        track?.setEnabled(false)
        peer?.setAudioPlayout(false)
        track = null
        peer = null
        abandonFocus()
        onState(null)
    }
}
