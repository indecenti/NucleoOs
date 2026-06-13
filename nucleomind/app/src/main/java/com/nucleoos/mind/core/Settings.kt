package com.nucleoos.mind.core

import android.content.Context
import android.content.SharedPreferences
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * Impostazioni persistenti (SharedPreferences) — sopravvivono al riavvio.
 * Lette sia dalla UI (reattive via StateFlow) sia dal server (getter diretti).
 */
object Settings {

    data class Data(
        val port: Int = 8080,
        val autoStartOnLaunch: Boolean = false,
        val autoStartOnBoot: Boolean = false,
        val autoReloadModel: Boolean = true,
        val lastModelFile: String = "",
        val apiKey: String = "",
        val temperature: Float = 0.8f,
        val topK: Int = 40,
        val maxTokens: Int = 512,
        val systemPrompt: String = "",
    )

    private const val PREFS = "nucleomind_settings"
    private lateinit var prefs: SharedPreferences

    private val _data = MutableStateFlow(Data())
    val data: StateFlow<Data> = _data

    fun init(context: Context) {
        prefs = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        _data.value = Data(
            port = prefs.getInt("port", 8080),
            autoStartOnLaunch = prefs.getBoolean("autoStartOnLaunch", false),
            autoStartOnBoot = prefs.getBoolean("autoStartOnBoot", false),
            autoReloadModel = prefs.getBoolean("autoReloadModel", true),
            lastModelFile = prefs.getString("lastModelFile", "") ?: "",
            apiKey = prefs.getString("apiKey", "") ?: "",
            temperature = prefs.getFloat("temperature", 0.8f),
            topK = prefs.getInt("topK", 40),
            maxTokens = prefs.getInt("maxTokens", 512),
            systemPrompt = prefs.getString("systemPrompt", "") ?: "",
        )
    }

    val current: Data get() = _data.value

    fun update(block: (Data) -> Data) {
        val next = block(_data.value)
        _data.value = next
        if (!::prefs.isInitialized) return
        prefs.edit()
            .putInt("port", next.port)
            .putBoolean("autoStartOnLaunch", next.autoStartOnLaunch)
            .putBoolean("autoStartOnBoot", next.autoStartOnBoot)
            .putBoolean("autoReloadModel", next.autoReloadModel)
            .putString("lastModelFile", next.lastModelFile)
            .putString("apiKey", next.apiKey)
            .putFloat("temperature", next.temperature)
            .putInt("topK", next.topK)
            .putInt("maxTokens", next.maxTokens)
            .putString("systemPrompt", next.systemPrompt)
            .apply()
    }
}
