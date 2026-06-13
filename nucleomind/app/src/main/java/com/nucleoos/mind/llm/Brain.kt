package com.nucleoos.mind.llm

import android.content.Context
import android.net.Uri
import com.nucleoos.mind.core.ServerState
import com.nucleoos.mind.core.Settings
import java.io.File

/**
 * Facciata sul motore corrente. Il server HTTP usa sempre [engine]; di default
 * è l'EchoEngine, finché l'utente non carica un modello reale.
 */
object Brain {

    @Volatile
    var engine: LlmEngine = EchoEngine
        private set

    private const val MODELS_DIR = "models"

    /** Costruisce il prompt da una lista di messaggi in stile chat. */
    fun buildPrompt(messages: List<Pair<String, String>>): String = buildString {
        for ((role, content) in messages) {
            when (role) {
                "system" -> append("Istruzioni: ").append(content).append("\n\n")
                "user" -> append("Utente: ").append(content).append("\n")
                "assistant" -> append("Assistente: ").append(content).append("\n")
                else -> append(content).append("\n")
            }
        }
        append("Assistente: ")
    }

    /**
     * Copia il modello selezionato (SAF Uri) nello storage privato dell'app e
     * lo carica in MediaPipe. MediaPipe richiede un path su filesystem reale.
     */
    fun loadFromUri(context: Context, uri: Uri, displayName: String, maxTokens: Int) {
        ServerState.update { it.copy(busy = true) }
        ServerState.log("Caricamento modello: $displayName …")
        try {
            val dir = File(context.filesDir, MODELS_DIR).apply { mkdirs() }
            val dest = File(dir, displayName)
            if (!dest.exists() || dest.length() == 0L) {
                context.contentResolver.openInputStream(uri).use { input ->
                    requireNotNull(input) { "Impossibile aprire il modello" }
                    dest.outputStream().use { output -> input.copyTo(output, 1 shl 20) }
                }
            }
            loadFromPath(context, dest.absolutePath, maxTokens)
        } catch (e: Exception) {
            ServerState.log("Errore caricamento: ${e.message}")
            ServerState.update { it.copy(busy = false) }
            throw e
        }
    }

    fun loadFromPath(context: Context, path: String, maxTokens: Int) {
        ServerState.update { it.copy(busy = true) }
        try {
            engine.close()
            val next = MediaPipeEngine.create(context, path, maxTokens)
            engine = next
            Settings.update { it.copy(lastModelFile = next.modelName) }
            ServerState.update {
                it.copy(
                    engineName = next.name,
                    modelLoaded = true,
                    modelName = next.modelName,
                    busy = false,
                )
            }
            ServerState.log("Modello pronto: ${next.modelName}")
        } catch (e: Exception) {
            engine = EchoEngine
            ServerState.update {
                it.copy(
                    engineName = EchoEngine.name,
                    modelLoaded = false,
                    modelName = EchoEngine.modelName,
                    busy = false,
                )
            }
            ServerState.log("Caricamento fallito: ${e.message}")
            throw e
        }
    }

    fun unload() {
        engine.close()
        engine = EchoEngine
        ServerState.update {
            it.copy(
                engineName = EchoEngine.name,
                modelLoaded = false,
                modelName = EchoEngine.modelName,
            )
        }
        ServerState.log("Modello scaricato.")
    }
}
