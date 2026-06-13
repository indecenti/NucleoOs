package com.nucleoos.mind.llm

/** Parametri di generazione, in stile OpenAI. */
data class GenParams(
    val temperature: Float = 0.8f,
    val topK: Int = 40,
    val maxTokens: Int = 512,
)

/**
 * Astrazione del motore di inferenza. Il server HTTP parla solo con questa
 * interfaccia: così possiamo avere un fallback "echo" che fa girare l'app
 * anche senza modello, e un motore reale (MediaPipe) quando un modello è caricato.
 */
interface LlmEngine {
    val name: String
    val modelName: String
    val ready: Boolean

    /**
     * Genera una risposta per [prompt]. Per ogni frammento prodotto invoca
     * [onToken] (utile per lo streaming SSE). Ritorna il testo completo.
     */
    fun generate(prompt: String, params: GenParams, onToken: (String) -> Unit): String

    /** Rilascia le risorse native, se presenti. */
    fun close() {}
}
