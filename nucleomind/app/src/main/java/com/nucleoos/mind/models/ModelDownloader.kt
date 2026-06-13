package com.nucleoos.mind.models

import android.content.Context
import com.nucleoos.mind.core.ServerState
import java.io.File
import java.io.RandomAccessFile
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Downloader per i modelli del catalogo. Uno alla volta, con ripresa (HTTP
 * Range) e progresso osservabile. Supporta un token Bearer facoltativo per i
 * repository gated (es. Hugging Face).
 */
object ModelDownloader {

    private val cancelFlag = AtomicBoolean(false)
    @Volatile private var active = false

    fun isActive() = active
    fun cancel() { cancelFlag.set(true) }

    /** Scarica [model]. Ritorna il File completato, o lancia in caso di errore. */
    fun download(context: Context, model: CatalogModel, bearerToken: String?): File {
        if (active) throw IllegalStateException("Un download è già in corso")
        active = true
        cancelFlag.set(false)
        ServerState.setDownload(ServerState.Download(model.id, model.name))
        ServerState.log("Download avviato: ${model.name}")

        val dir = ModelRepository.dir(context)
        val part = File(dir, model.fileName + ".part")
        val out = File(dir, model.fileName)

        try {
            var conn = open(model.url, bearerToken, existing = part.length())
            // Alcuni host rispondono 200 ignorando il Range: ripartiamo da zero.
            var resumeFrom = part.length()
            if (conn.responseCode == HttpURLConnection.HTTP_OK) resumeFrom = 0L
            if (conn.responseCode !in 200..299) {
                val code = conn.responseCode
                conn.disconnect()
                if (code == 401 || code == 403) {
                    throw IllegalStateException("Accesso negato ($code): modello gated, serve token/licenza.")
                }
                throw IllegalStateException("HTTP $code dal server modelli.")
            }

            val contentLen = conn.contentLengthLong.coerceAtLeast(0)
            val total = if (resumeFrom > 0) resumeFrom + contentLen else contentLen

            RandomAccessFile(part, "rw").use { raf ->
                raf.seek(resumeFrom)
                conn.inputStream.use { input ->
                    val buf = ByteArray(1 shl 16)
                    var received = resumeFrom
                    var lastUi = 0L
                    while (true) {
                        if (cancelFlag.get()) throw InterruptedException("Annullato")
                        val n = input.read(buf)
                        if (n < 0) break
                        raf.write(buf, 0, n)
                        received += n
                        if (received - lastUi > (1 shl 20)) { // aggiorna UI ~ogni MB
                            lastUi = received
                            ServerState.setDownload(
                                ServerState.Download(model.id, model.name, received, total)
                            )
                        }
                    }
                }
            }
            conn.disconnect()

            if (out.exists()) out.delete()
            if (!part.renameTo(out)) throw IllegalStateException("Impossibile finalizzare il file")

            ServerState.setDownload(
                ServerState.Download(model.id, model.name, out.length(), out.length(), done = true)
            )
            ServerState.log("Download completato: ${model.name}")
            ModelRepository.refresh(context)
            return out
        } catch (e: Exception) {
            ServerState.setDownload(
                ServerState.Download(model.id, model.name, error = e.message ?: "errore")
            )
            ServerState.log("Download fallito: ${e.message}")
            throw e
        } finally {
            active = false
        }
    }

    private fun open(url: String, token: String?, existing: Long): HttpURLConnection {
        val conn = URL(url).openConnection() as HttpURLConnection
        conn.connectTimeout = 15000
        conn.readTimeout = 30000
        conn.instanceFollowRedirects = true
        if (!token.isNullOrBlank()) conn.setRequestProperty("Authorization", "Bearer $token")
        if (existing > 0) conn.setRequestProperty("Range", "bytes=$existing-")
        conn.connect()
        return conn
    }
}
