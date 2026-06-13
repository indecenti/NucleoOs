package com.nucleoos.mind.llm

import android.content.Context

/**
 * STUB per la build locale (AGP 7.4 / JDK 11): la dipendenza MediaPipe
 * tasks-genai è temporaneamente esclusa perché il vecchio dexer D8 non riesce a
 * convertire il suo bytecode. L'app gira con EchoEngine. Nella configurazione
 * moderna (AGP 8.7 / JDK 17) questo file torna a usare com.google.mediapipe.
 *
 * Stessa API pubblica della versione reale: Brain.create() lancia, Brain fa
 * fallback su EchoEngine.
 */
class MediaPipeEngine private constructor(
    override val modelName: String,
) : LlmEngine {

    override val name = "mediapipe"
    override val ready = true

    override fun generate(prompt: String, params: GenParams, onToken: (String) -> Unit): String {
        throw UnsupportedOperationException("MediaPipe non disponibile in questa build")
    }

    companion object {
        fun create(context: Context, modelPath: String, maxTokens: Int): MediaPipeEngine {
            throw UnsupportedOperationException(
                "Motore MediaPipe escluso da questa build locale (dexer D8 di AGP 7.4). " +
                    "Disponibile nella build moderna AGP 8.7 / JDK 17."
            )
        }
    }
}
