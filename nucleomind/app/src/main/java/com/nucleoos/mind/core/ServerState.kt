package com.nucleoos.mind.core

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update

/**
 * Stato globale osservabile dell'app: condiviso tra il foreground service
 * (che possiede il server HTTP) e la UI Compose. Un solo punto di verità.
 */
object ServerState {

    data class Snapshot(
        // Server
        val running: Boolean = false,
        val ip: String = "-",
        val port: Int = DEFAULT_PORT,
        val startedAtElapsed: Long = 0L,
        // Motore / modello
        val engineName: String = "echo",
        val modelLoaded: Boolean = false,
        val modelName: String = "(nessun modello)",
        val busy: Boolean = false,
        // Metriche
        val requestsServed: Long = 0,
        val tokensGenerated: Long = 0,
        val lastTokensPerSec: Double = 0.0,
        // Dispositivo
        val ramTotalMb: Int = 0,
        val ramAvailMb: Int = 0,
        val batteryPct: Int = -1,
        val charging: Boolean = false,
        val thermal: String = "—",
        val cpuCores: Int = Runtime.getRuntime().availableProcessors(),
        // Guard "respirazione del sistema"
        val throttled: Boolean = false,
        val throttleReason: String = "",
    )

    data class ReqLog(
        val endpoint: String,
        val ms: Long,
        val tokens: Int,
        val ok: Boolean,
    )

    data class Download(
        val id: String,
        val name: String,
        val receivedBytes: Long = 0,
        val totalBytes: Long = 0,
        val done: Boolean = false,
        val error: String? = null,
    ) {
        val pct: Int get() = if (totalBytes > 0) ((receivedBytes * 100) / totalBytes).toInt() else 0
    }

    const val DEFAULT_PORT = 8080
    private const val MAX_LOG_LINES = 200

    private val _state = MutableStateFlow(Snapshot())
    val state: StateFlow<Snapshot> = _state

    private val _logs = MutableStateFlow<List<String>>(emptyList())
    val logs: StateFlow<List<String>> = _logs

    /** Download in corso (null = nessuno). Uno solo alla volta. */
    private val _download = MutableStateFlow<Download?>(null)
    val download: StateFlow<Download?> = _download

    /** Modelli installati nello storage privato (nomi file). */
    private val _installed = MutableStateFlow<List<String>>(emptyList())
    val installed: StateFlow<List<String>> = _installed

    /** Storico ultime richieste servite. */
    private val _history = MutableStateFlow<List<ReqLog>>(emptyList())
    val history: StateFlow<List<ReqLog>> = _history

    fun addReqLog(r: ReqLog) {
        _history.update { (listOf(r) + it).take(20) }
    }

    fun update(block: (Snapshot) -> Snapshot) = _state.update(block)
    fun setDownload(d: Download?) { _download.value = d }
    fun setInstalled(list: List<String>) { _installed.value = list }

    fun log(line: String) {
        _logs.update { prev ->
            val next = prev + line
            if (next.size > MAX_LOG_LINES) next.takeLast(MAX_LOG_LINES) else next
        }
    }

    fun clearLogs() { _logs.value = emptyList() }
}
