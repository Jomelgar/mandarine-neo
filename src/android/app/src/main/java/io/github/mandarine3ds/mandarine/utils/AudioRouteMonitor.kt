// Copyright 2026 Citra Project / Mandarine Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package io.github.mandarine3ds.mandarine.utils

import android.content.Context
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Handler
import android.os.Looper

/**
 * Watches Android audio routing and notifies when the primary output changes.
 *
 * The emulator keeps its own audio stream open, so when the system switches between the
 * speaker, wired headphones, Bluetooth or USB it must be reopened to follow the new route.
 */
class AudioRouteMonitor(
    context: Context,
    private val onRouteChanged: () -> Unit
) {
    private val audioManager =
        context.applicationContext.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private val handler = Handler(Looper.getMainLooper())
    private var registered = false

    private val deviceCallback = object : AudioDeviceCallback() {
        override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>) {
            logOutputDevices("added", addedDevices)
            scheduleRestart()
        }

        override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>) {
            logOutputDevices("removed", removedDevices)
            scheduleRestart()
        }
    }

    fun start() {
        if (registered) {
            return
        }
        audioManager.registerAudioDeviceCallback(deviceCallback, handler)
        registered = true
        Log.info("[AudioRouteMonitor] Output devices: ${describeOutputs()}")
    }

    fun stop() {
        if (!registered) {
            return
        }
        audioManager.unregisterAudioDeviceCallback(deviceCallback)
        handler.removeCallbacksAndMessages(null)
        registered = false
    }

    private fun scheduleRestart() {
        // Give Android time to finish switching the route before reopening the stream.
        handler.removeCallbacksAndMessages(null)
        handler.postDelayed({
            Log.info("[AudioRouteMonitor] Output devices: ${describeOutputs()}")
            onRouteChanged()
        }, ROUTE_SETTLE_DELAY_MS)
    }

    private fun describeOutputs(): String {
        val outputs = audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS).filter {
            it.isSink
        }
        return if (outputs.isEmpty()) "none" else outputs.joinToString { it.typeName }
    }

    private fun logOutputDevices(event: String, devices: Array<out AudioDeviceInfo>) {
        val sinks = devices.filter { it.isSink }.joinToString { it.typeName }
        if (sinks.isNotEmpty()) {
            Log.info("[AudioRouteMonitor] Output devices $event: $sinks")
        }
    }

    companion object {
        private const val ROUTE_SETTLE_DELAY_MS = 300L
    }
}
