package com.nucleoos.mind.models

/**
 * Catalogo curato di piccoli LLM compatibili con MediaPipe LLM Inference
 * (formato .task). I file ufficiali sono distribuiti dalla community LiteRT su
 * Hugging Face; alcuni richiedono di accettare la licenza (token HF facoltativo
 * nell'app). In ogni caso resta sempre disponibile "Da file" e "Da URL".
 */
data class CatalogModel(
    val id: String,
    val name: String,
    val params: String,
    val approxSizeMb: Int,
    val ramNeededMb: Int,
    val family: String,
    val url: String,
    val fileName: String,
    val gated: Boolean,
    val note: String = "",
)

object ModelCatalog {
    val models: List<CatalogModel> = listOf(
        CatalogModel(
            id = "gemma3-1b-it-q4",
            name = "Gemma 3 1B IT (int4)",
            params = "1B",
            approxSizeMb = 555,
            ramNeededMb = 1600,
            family = "Gemma",
            url = "https://huggingface.co/litert-community/Gemma3-1B-IT/resolve/main/Gemma3-1B-IT_multi-prefill-seq_q4_ekv2048.task",
            fileName = "Gemma3-1B-IT_q4.task",
            gated = true,
            note = "Ottimo compromesso per telefoni con 4GB+.",
        ),
        CatalogModel(
            id = "gemma2-2b-it-q8",
            name = "Gemma 2 2B IT (int8)",
            params = "2B",
            approxSizeMb = 2630,
            ramNeededMb = 3500,
            family = "Gemma",
            url = "https://huggingface.co/litert-community/Gemma2-2B-IT/resolve/main/Gemma2-2B-IT_multi-prefill-seq_q8_ekv1280.task",
            fileName = "Gemma2-2B-IT_q8.task",
            gated = true,
            note = "Più capace; consigliato 6GB+ di RAM.",
        ),
        CatalogModel(
            id = "phi2-q8",
            name = "Phi-2 (int8)",
            params = "2.7B",
            approxSizeMb = 3080,
            ramNeededMb = 4000,
            family = "Phi",
            url = "https://huggingface.co/litert-community/Phi-2/resolve/main/Phi-2_multi-prefill-seq_q8_ekv1280.task",
            fileName = "Phi-2_q8.task",
            gated = true,
            note = "Forte sul ragionamento; pesante.",
        ),
        CatalogModel(
            id = "tinyllama-1.1b-q8",
            name = "TinyLlama 1.1B Chat (int8)",
            params = "1.1B",
            approxSizeMb = 1200,
            ramNeededMb = 2000,
            family = "Llama",
            url = "https://huggingface.co/litert-community/TinyLlama-1.1B-Chat-v1.0/resolve/main/TinyLlama-1.1B-Chat-v1.0_multi-prefill-seq_q8_ekv1280.task",
            fileName = "TinyLlama-1.1B_q8.task",
            gated = false,
            note = "Leggero, parte ovunque.",
        ),
    )

    fun byId(id: String): CatalogModel? = models.firstOrNull { it.id == id }
}
