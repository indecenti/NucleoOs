package com.nucleoos.mind.service

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import com.nucleoos.mind.core.Settings

/**
 * Avvia il server al boot, se l'utente l'ha abilitato. Su Android 14+ l'avvio
 * di un foreground service dal boot può essere limitato dal sistema: in tal caso
 * fallisce in silenzio e l'utente lo avvia manualmente.
 */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED) return
        Settings.init(context)
        if (!Settings.current.autoStartOnBoot) return
        try {
            ServerService.start(context)
        } catch (_: Exception) {
        }
    }
}
