package com.nucleoos.mind

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.database.Cursor
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.OpenableColumns
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import com.nucleoos.mind.core.DeviceInfo
import com.nucleoos.mind.core.ServerState
import com.nucleoos.mind.core.Settings
import com.nucleoos.mind.llm.Brain
import com.nucleoos.mind.models.ModelRepository
import com.nucleoos.mind.service.ServerService
import com.nucleoos.mind.ui.NucleoMindApp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
            != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 100)
        }

        // Stato iniziale: settings, modelli installati + risorse del dispositivo
        // (per le raccomandazioni) anche prima di avviare il server.
        Settings.init(this)
        ModelRepository.refresh(this)
        primeDeviceInfo()
        maybeAutoStart()

        setContent {
            val pickModel = rememberLauncherForActivityResult(
                ActivityResultContracts.OpenDocument()
            ) { uri: Uri? -> if (uri != null) onModelPicked(uri) }

            NucleoMindApp(
                onStart = { ServerService.start(this) },
                onStop = { ServerService.stop(this) },
                onPickModel = { pickModel.launch(arrayOf("*/*")) },
            )
        }
    }

    /** Ricarica l'ultimo modello e avvia il server, se abilitato nelle Settings. */
    private fun maybeAutoStart() {
        val s = Settings.current
        if (s.autoReloadModel && s.lastModelFile.isNotBlank()) {
            val f = ModelRepository.dir(this).resolve(s.lastModelFile)
            if (f.exists() && f.length() > 0) {
                lifecycleScope.launch {
                    withContext(Dispatchers.IO) {
                        runCatching {
                            Brain.loadFromPath(applicationContext, f.absolutePath, s.maxTokens)
                        }
                    }
                }
            }
        }
        if (s.autoStartOnLaunch) ServerService.start(this)
    }

    private fun primeDeviceInfo() {
        val d = DeviceInfo.sample(this)
        ServerState.update {
            it.copy(
                ramTotalMb = d.ramTotalMb, ramAvailMb = d.ramAvailMb,
                batteryPct = d.batteryPct, charging = d.charging, thermal = d.thermal
            )
        }
    }

    private fun onModelPicked(uri: Uri) {
        try {
            contentResolver.takePersistableUriPermission(
                uri, Intent.FLAG_GRANT_READ_URI_PERMISSION
            )
        } catch (_: Exception) {
        }
        val name = queryDisplayName(uri) ?: "model.task"
        lifecycleScope.launch {
            withContext(Dispatchers.IO) {
                runCatching {
                    Brain.loadFromUri(applicationContext, uri, name, maxTokens = 1024)
                    ModelRepository.refresh(applicationContext)
                }
            }
        }
    }

    private fun queryDisplayName(uri: Uri): String? {
        var cursor: Cursor? = null
        return try {
            cursor = contentResolver.query(uri, null, null, null, null)
            val idx = cursor?.getColumnIndex(OpenableColumns.DISPLAY_NAME) ?: -1
            if (cursor != null && idx >= 0 && cursor.moveToFirst()) cursor.getString(idx) else null
        } catch (_: Exception) {
            null
        } finally {
            cursor?.close()
        }
    }
}
