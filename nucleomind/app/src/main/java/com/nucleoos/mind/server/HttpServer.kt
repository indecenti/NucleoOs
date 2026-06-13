package com.nucleoos.mind.server

import com.nucleoos.mind.core.ServerState
import com.nucleoos.mind.core.Settings
import com.nucleoos.mind.llm.Brain
import com.nucleoos.mind.llm.GenParams
import fi.iki.elonen.NanoHTTPD
import org.json.JSONArray
import org.json.JSONObject
import java.io.PipedInputStream
import java.io.PipedOutputStream

/**
 * Server HTTP che espone un'API compatibile OpenAI sulla LAN.
 * Endpoint:
 *   GET  /health                 -> stato
 *   GET  /v1/models              -> modello corrente
 *   POST /v1/chat/completions    -> chat (supporta "stream": true)
 */
class HttpServer(port: Int) : NanoHTTPD("0.0.0.0", port) {

    override fun serve(session: IHTTPSession): Response {
        return try {
            when {
                session.uri == "/health" -> health()
                session.uri == "/metrics" -> metrics()
                session.uri == "/v1/models" -> models()
                session.uri == "/" -> root()
                session.uri == "/v1/chat/completions" && session.method == Method.POST ->
                    chatCompletions(session)
                session.uri == "/v1/distill" && session.method == Method.POST ->
                    distill(session)
                session.uri == "/v1/ground" && session.method == Method.POST ->
                    ground(session)
                else -> json(Response.Status.NOT_FOUND, JSONObject().put("error", "not found"))
            }
        } catch (e: Exception) {
            ServerState.log("Errore richiesta: ${e.message}")
            json(
                Response.Status.INTERNAL_ERROR,
                JSONObject().put("error", e.message ?: "internal error")
            )
        }
    }

    private fun root(): Response {
        val s = ServerState.state.value
        val body = JSONObject()
            .put("service", "NucleoMind")
            .put("version", "2.0.0")
            .put("engine", s.engineName)
            .put("model", s.modelName)
            .put("auth_required", Settings.current.apiKey.isNotBlank())
            .put(
                "endpoints", JSONArray()
                    .put("/health").put("/metrics").put("/v1/models")
                    .put("/v1/chat/completions").put("/v1/distill").put("/v1/ground")
            )
        return json(Response.Status.OK, body)
    }

    private fun health(): Response {
        val s = ServerState.state.value
        val body = JSONObject()
            .put("status", "ok")
            .put("engine", s.engineName)
            .put("model_loaded", s.modelLoaded)
            .put("busy", s.busy)
        return json(Response.Status.OK, body)
    }

    private fun metrics(): Response {
        val s = ServerState.state.value
        val uptime = if (s.startedAtElapsed > 0)
            (android.os.SystemClock.elapsedRealtime() - s.startedAtElapsed) / 1000 else 0
        val body = JSONObject()
            .put("uptime_s", uptime)
            .put("requests_served", s.requestsServed)
            .put("tokens_generated", s.tokensGenerated)
            .put("tokens_per_sec", s.lastTokensPerSec)
            .put("ram_total_mb", s.ramTotalMb)
            .put("ram_avail_mb", s.ramAvailMb)
            .put("battery_pct", s.batteryPct)
            .put("charging", s.charging)
            .put("thermal", s.thermal)
            .put("throttled", s.throttled)
            .put("throttle_reason", s.throttleReason)
        return json(Response.Status.OK, body)
    }

    /**
     * Endpoint "teacher": il telefono fa da maestro. Riceve un argomento e
     * restituisce una card fattuale bilingue pronta da ingerire nel cervello
     * permanente di ANIMA (flywheel di conoscenza). Funzione innovativa: la
     * conoscenza fluisce dal grande (telefono) al piccolo (Cardputer) e resta.
     */
    private fun distill(session: IHTTPSession): Response {
        unauthorized(session)?.let { return it }
        throttledResponse()?.let { return it }
        val start = System.nanoTime()
        val map = HashMap<String, String>()
        session.parseBody(map)
        val req = JSONObject(map["postData"] ?: "{}")
        val topic = req.optString("topic", req.optString("question", "")).trim()
        if (topic.isEmpty()) {
            return json(Response.Status.BAD_REQUEST, JSONObject().put("error", "topic mancante"))
        }
        val lang = req.optString("lang", "it")
        val instruction = if (lang == "en")
            "In ONE concise factual sentence, state the key fact about: \"$topic\". No preamble."
        else
            "In UNA frase concisa e fattuale, enuncia il fatto chiave su: \"$topic\". Nessun preambolo."

        ServerState.update { it.copy(requestsServed = it.requestsServed + 1, busy = true) }
        var tokens = 0
        val answer = Brain.engine.generate(instruction, GenParams(maxTokens = 160)) { tokens++ }.trim()
        ServerState.update { it.copy(busy = false) }
        ServerState.addReqLog(ServerState.ReqLog("/v1/distill", ms(start), tokens, true))

        val card = JSONObject()
            .put("topic", topic)
            .put("lang", lang)
            .put("fact", answer)
            .put("source", "nucleomind:${ServerState.state.value.modelName}")
        return json(Response.Status.OK, JSONObject().put("card", card))
    }

    /**
     * Endpoint "grounded" (split-inference): il client (Cardputer) fa il
     * retrieval localmente e invia SOLO l'evidenza già selezionata; il telefono
     * sintetizza esclusivamente da quella, con vincolo anti-allucinazione. Il
     * grounding resta sul client, la fluidificazione sul telefono.
     */
    private fun ground(session: IHTTPSession): Response {
        unauthorized(session)?.let { return it }
        throttledResponse()?.let { return it }
        val start = System.nanoTime()
        val map = HashMap<String, String>()
        session.parseBody(map)
        val req = JSONObject(map["postData"] ?: "{}")
        val question = req.optString("question").trim()
        val evidenceArr = req.optJSONArray("evidence") ?: JSONArray()
        if (question.isEmpty() || evidenceArr.length() == 0) {
            return json(Response.Status.BAD_REQUEST, JSONObject().put("error", "servono 'question' ed 'evidence'"))
        }
        val lang = req.optString("lang", "it")
        val evidence = buildString {
            for (i in 0 until evidenceArr.length()) {
                append("[").append(i + 1).append("] ").append(evidenceArr.optString(i)).append("\n")
            }
        }
        val prompt = if (lang == "en")
            "Answer the question using ONLY the evidence below. If the evidence does not " +
                "contain the answer, reply exactly \"I don't know\". Be concise.\n\n" +
                "Evidence:\n$evidence\nQuestion: $question\nAnswer:"
        else
            "Rispondi alla domanda usando SOLO l'evidenza qui sotto. Se l'evidenza non " +
                "contiene la risposta, scrivi esattamente \"Non lo so\". Sii conciso.\n\n" +
                "Evidenza:\n$evidence\nDomanda: $question\nRisposta:"

        ServerState.update { it.copy(requestsServed = it.requestsServed + 1, busy = true) }
        var tokens = 0
        val answer = Brain.engine.generate(prompt, settingsParams(req)) { tokens++ }.trim()
        recordRate(start, tokens)
        ServerState.update { it.copy(busy = false) }
        ServerState.addReqLog(ServerState.ReqLog("/v1/ground", ms(start), tokens, true))

        val grounded = answer.isNotEmpty() &&
            !answer.equals("non lo so", true) && !answer.equals("i don't know", true)
        val body = JSONObject()
            .put("answer", answer)
            .put("grounded", grounded)
            .put("evidence_count", evidenceArr.length())
        return json(Response.Status.OK, body)
    }

    /** Parametri di generazione: default da Settings, sovrascritti dalla richiesta. */
    private fun settingsParams(req: JSONObject): GenParams {
        val s = Settings.current
        return GenParams(
            temperature = req.optDouble("temperature", s.temperature.toDouble()).toFloat(),
            topK = req.optInt("top_k", s.topK),
            maxTokens = req.optInt("max_tokens", s.maxTokens),
        )
    }

    /** Verifica l'API key se impostata. Ritorna 401 altrimenti, null se ok. */
    private fun unauthorized(session: IHTTPSession): Response? {
        val key = Settings.current.apiKey
        if (key.isBlank()) return null
        val auth = session.headers["authorization"]?.removePrefix("Bearer ")?.removePrefix("bearer ")
        val xkey = session.headers["x-api-key"]
        if (auth?.trim() == key || xkey?.trim() == key) return null
        return json(Response.Status.UNAUTHORIZED, JSONObject().put("error", "api key non valida"))
    }

    private fun ms(startNano: Long): Long = (System.nanoTime() - startNano) / 1_000_000

    /** Se il dispositivo è sotto stress, declina il lavoro pesante (503). */
    private fun throttledResponse(): Response? {
        val s = ServerState.state.value
        if (!s.throttled) return null
        val body = JSONObject()
            .put("error", "device throttled")
            .put("reason", s.throttleReason)
        return json(Response.Status.SERVICE_UNAVAILABLE, body)
    }

    private fun models(): Response {
        val s = ServerState.state.value
        val data = JSONArray().put(
            JSONObject()
                .put("id", if (s.modelLoaded) s.modelName else "nucleomind-echo")
                .put("object", "model")
                .put("owned_by", "nucleomind")
        )
        return json(Response.Status.OK, JSONObject().put("object", "list").put("data", data))
    }

    private fun chatCompletions(session: IHTTPSession): Response {
        unauthorized(session)?.let { return it }
        throttledResponse()?.let { return it }
        val map = HashMap<String, String>()
        session.parseBody(map)
        val raw = map["postData"] ?: "{}"
        val req = JSONObject(raw)

        val messages = mutableListOf<Pair<String, String>>()
        val arr = req.optJSONArray("messages") ?: JSONArray()
        for (i in 0 until arr.length()) {
            val m = arr.getJSONObject(i)
            messages.add(m.optString("role", "user") to m.optString("content", ""))
        }
        // System prompt da Settings, se la richiesta non ne ha già uno.
        val sys = Settings.current.systemPrompt
        if (sys.isNotBlank() && messages.none { it.first == "system" }) {
            messages.add(0, "system" to sys)
        }
        val params = settingsParams(req)
        val stream = req.optBoolean("stream", false)
        val prompt = Brain.buildPrompt(messages)
        val model = ServerState.state.value.modelName

        ServerState.update { it.copy(requestsServed = it.requestsServed + 1, busy = true) }
        ServerState.log("chat: ${messages.lastOrNull()?.second?.take(60) ?: ""}")

        return if (stream) streamChat(prompt, params, model) else blockingChat(prompt, params, model)
    }

    private fun blockingChat(prompt: String, params: GenParams, model: String): Response {
        val start = System.nanoTime()
        var tokens = 0
        val text = Brain.engine.generate(prompt, params) { tokens++ }
        recordRate(start, tokens)
        ServerState.addReqLog(ServerState.ReqLog("/v1/chat/completions", ms(start), tokens, true))

        val body = JSONObject()
            .put("id", "chatcmpl-${System.nanoTime()}")
            .put("object", "chat.completion")
            .put("model", model)
            .put(
                "choices", JSONArray().put(
                    JSONObject()
                        .put("index", 0)
                        .put(
                            "message",
                            JSONObject().put("role", "assistant").put("content", text)
                        )
                        .put("finish_reason", "stop")
                )
            )
        ServerState.update { it.copy(busy = false) }
        return json(Response.Status.OK, body)
    }

    private fun streamChat(prompt: String, params: GenParams, model: String): Response {
        val pin = PipedInputStream(1 shl 16)
        val pout = PipedOutputStream(pin)
        val id = "chatcmpl-${System.nanoTime()}"

        Thread {
            val start = System.nanoTime()
            var tokens = 0
            try {
                Brain.engine.generate(prompt, params) { tok ->
                    tokens++
                    val chunk = JSONObject()
                        .put("id", id)
                        .put("object", "chat.completion.chunk")
                        .put("model", model)
                        .put(
                            "choices", JSONArray().put(
                                JSONObject()
                                    .put("index", 0)
                                    .put("delta", JSONObject().put("content", tok))
                            )
                        )
                    pout.write("data: $chunk\n\n".toByteArray())
                    pout.flush()
                }
                val done = JSONObject()
                    .put("id", id)
                    .put("object", "chat.completion.chunk")
                    .put("model", model)
                    .put(
                        "choices", JSONArray().put(
                            JSONObject().put("index", 0)
                                .put("delta", JSONObject())
                                .put("finish_reason", "stop")
                        )
                    )
                pout.write("data: $done\n\n".toByteArray())
                pout.write("data: [DONE]\n\n".toByteArray())
                pout.flush()
            } catch (e: Exception) {
                ServerState.log("Errore stream: ${e.message}")
            } finally {
                recordRate(start, tokens)
                ServerState.addReqLog(ServerState.ReqLog("/v1/chat/completions", ms(start), tokens, true))
                ServerState.update { it.copy(busy = false) }
                try {
                    pout.close()
                } catch (_: Exception) {
                }
            }
        }.apply { isDaemon = true }.start()

        val resp = newChunkedResponse(Response.Status.OK, "text/event-stream", pin)
        resp.addHeader("Cache-Control", "no-cache")
        resp.addHeader("Connection", "keep-alive")
        resp.addHeader("Access-Control-Allow-Origin", "*")
        return resp
    }

    private fun recordRate(startNano: Long, tokens: Int) {
        val secs = (System.nanoTime() - startNano) / 1e9
        val rate = if (secs > 0) tokens / secs else 0.0
        ServerState.update {
            it.copy(lastTokensPerSec = rate, tokensGenerated = it.tokensGenerated + tokens)
        }
    }

    private fun json(status: Response.Status, obj: JSONObject): Response {
        val resp = newFixedLengthResponse(status, "application/json", obj.toString())
        resp.addHeader("Access-Control-Allow-Origin", "*")
        return resp
    }
}
