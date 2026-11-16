// Copyright 2026 Citra Project / Mandarine Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package io.github.mandarine3ds.mandarine.utils

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Handler
import android.os.Looper

/**
 * Requests audio focus for the emulator and reports focus changes on the main thread.
 */
class AudioFocusHelper(
    context: Context,
    private val onFocusGained: () -> Unit,
    private val onFocusLost: () -> Unit
) {
    private val audioManager =
        context.applicationContext.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private val handler = Handler(Looper.getMainLooper())
    private var hasFocus = false

    private val focusRequest: AudioFocusRequest =
        AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_GAME)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build()
            )
            .setWillPauseWhenDucked(false)
            .setOnAudioFocusChangeListener { focusChange ->
                handler.post { handleFocusChange(focusChange) }
            }
            .build()

    fun requestFocus(): Boolean {
        if (hasFocus) {
            return true
        }

        val result = audioManager.requestAudioFocus(focusRequest)
        hasFocus = result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
        if (!hasFocus) {
            Log.warning("[AudioFocusHelper] Audio focus request denied")
        }
        return hasFocus
    }

    fun abandonFocus() {
        if (!hasFocus) {
            return
        }

        audioManager.abandonAudioFocusRequest(focusRequest)
        hasFocus = false
    }

    private fun handleFocusChange(focusChange: Int) {
        when (focusChange) {
            AudioManager.AUDIOFOCUS_GAIN -> {
                hasFocus = true
                Log.info("[AudioFocusHelper] Audio focus gained")
                onFocusGained()
            }

            AudioManager.AUDIOFOCUS_LOSS,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> {
                hasFocus = false
                Log.warning("[AudioFocusHelper] Audio focus lost")
                onFocusLost()
            }
        }
    }
}
