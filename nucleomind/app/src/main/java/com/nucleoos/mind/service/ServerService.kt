package com.nucleoos.mind.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.os.SystemClock
import androidx.lifecycle.LifecycleService
import androidx.lifecycle.lifecycleScope
import com.nucleoos.mind.MainActivity
import com.nucleoos.mind.core.DeviceInfo
import com.nucleoos.mind.core.Net
import com.nucleoos.mind.core.ServerState
import com.nucleoos.mind.core.Settings
import com.nucleoos.mind.discovery.MdnsAdvertiser
import com.nucleoos.mind.server.HttpServer
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * Foreground service che tiene vivo il server LLM mentre l'app è in background.
 * Possiede il ciclo di vita del server HTTP e dell'annuncio mDNS.
 */
class ServerService : LifecycleService() {

    private var server: HttpServer? = null
    private var mdns: MdnsAdvertiser? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var monitorJob: Job? = null

    override fun onBind(intent: Intent): IBinder? {
        super.onBind(intent)
        return null
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        super.onStartCommand(intent, flags, startId)
        when (intent?.action) {
            ACTION_STOP -> {
                stopEverything()
                stopSelf()
                return START_NOT_STICKY
            }
            else -> startEverything()
        }
        return START_STICKY
    }

    private fun startEverything() {
        if (server != null) return
        Settings.init(this)
        val port = Settings.current.port
        val ip = Net.localIpv4()

        startForeground(NOTIF_ID, buildNotification(ip, port))

        try {
            server = HttpServer(port).also { it.start(NanoHttpdTimeout, false) }
            mdns = MdnsAdvertiser(this).also { it.register(port) }
            acquireWakeLock()
            ServerState.update {
                it.copy(running = true, ip = ip, port = port, startedAtElapsed = SystemClock.elapsedRealtime())
            }
            ServerState.log("Server avviato su http://$ip:$port")
            startMonitor()
        } catch (e: Exception) {
            ServerState.log("Avvio fallito: ${e.message}")
            stopEverything()
            stopSelf()
        }
    }

    private fun stopEverything() {
        monitorJob?.cancel(); monitorJob = null
        mdns?.unregister(); mdns = null
        server?.stop(); server = null
        releaseWakeLock()
        ServerState.update { it.copy(running = false, throttled = false, throttleReason = "") }
        ServerState.log("Server fermato.")
    }

    private fun acquireWakeLock() {
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "NucleoMind::server").apply {
            setReferenceCounted(false)
            acquire(6 * 60 * 60 * 1000L) // safety cap 6h
        }
    }

    private fun releaseWakeLock() {
        try {
            if (wakeLock?.isHeld == true) wakeLock?.release()
        } catch (_: Exception) {
        }
        wakeLock = null
    }

    /**
     * Monitor del dispositivo + guard "respirazione del sistema": quando la
     * batteria è bassa (e non in carica) o il telefono scotta, il server resta
     * vivo ma declina il lavoro pesante (503), poi riprende da solo.
     */
    private fun startMonitor() {
        monitorJob = lifecycleScope.launch {
            while (isActive) {
                val d = DeviceInfo.sample(this@ServerService)
                val lowBattery = d.batteryPct in 0..14 && !d.charging
                val throttle = lowBattery || d.thermalSevere
                val reason = when {
                    d.thermalSevere -> "temperatura ${d.thermal}"
                    lowBattery -> "batteria ${d.batteryPct}%"
                    else -> ""
                }
                val was = ServerState.state.value.throttled
                ServerState.update {
                    it.copy(
                        ramTotalMb = d.ramTotalMb,
                        ramAvailMb = d.ramAvailMb,
                        batteryPct = d.batteryPct,
                        charging = d.charging,
                        thermal = d.thermal,
                        throttled = throttle,
                        throttleReason = reason,
                    )
                }
                if (throttle && !was) ServerState.log("Guard attivo: $reason → declino richieste pesanti")
                if (!throttle && was) ServerState.log("Guard rilasciato: dispositivo di nuovo ok")
                delay(3000)
            }
        }
    }

    override fun onDestroy() {
        stopEverything()
        super.onDestroy()
    }

    private fun buildNotification(ip: String, port: Int): Notification {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val ch = NotificationChannel(
                CHANNEL_ID, "NucleoMind server", NotificationManager.IMPORTANCE_LOW
            )
            nm.createNotificationChannel(ch)
        }
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )
        val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(this, CHANNEL_ID)
        } else {
            @Suppress("DEPRECATION") Notification.Builder(this)
        }
        return builder
            .setContentTitle("NucleoMind attivo")
            .setContentText("Server LLM su http://$ip:$port")
            .setSmallIcon(android.R.drawable.ic_menu_compass)
            .setOngoing(true)
            .setContentIntent(open)
            .build()
    }

    companion object {
        const val ACTION_START = "com.nucleoos.mind.START"
        const val ACTION_STOP = "com.nucleoos.mind.STOP"
        private const val CHANNEL_ID = "nucleomind_server"
        private const val NOTIF_ID = 1
        private const val NanoHttpdTimeout = 10_000

        fun start(context: Context) {
            val i = Intent(context, ServerService::class.java).setAction(ACTION_START)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(i)
            } else {
                context.startService(i)
            }
        }

        fun stop(context: Context) {
            context.startService(Intent(context, ServerService::class.java).setAction(ACTION_STOP))
        }
    }
}
