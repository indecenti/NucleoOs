package com.nucleoos.mind.llm

/**
 * Motore di fallback: nessun modello, nessuna dipendenza. Serve a far girare
 * e testare l'intera pipeline (server, mDNS, client Cardputer) prima ancora di
 * caricare un modello reale. Riconosce /health-style ping e fa da eco ragionata.
 */
object EchoEngine : LlmEngine {
    override val name = "echo"
    override val modelName = "(nessun modello — fallback echo)"
    override val ready = true

    override fun generate(prompt: String, params: GenParams, onToken: (String) -> Unit): String {
        val reply = buildString {
            append("NucleoMind è attivo, ma non è ancora caricato alcun modello. ")
            append("Carica un modello .task/.bin dalla schermata principale per avere ")
            append("inferenza reale. Ho ricevuto ")
            append(prompt.trim().take(200).ifEmpty { "(prompt vuoto)" })
        }
        // Simula lo streaming token-per-token a parole.
        for (word in reply.split(" ")) {
            onToken("$word ")
        }
        return reply
    }
}
