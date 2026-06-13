package com.nucleoos.mind.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.nucleoos.mind.core.ServerState
import com.nucleoos.mind.llm.Brain
import com.nucleoos.mind.models.CatalogModel
import com.nucleoos.mind.models.ModelCatalog
import com.nucleoos.mind.models.ModelDownloader
import com.nucleoos.mind.models.ModelRepository
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

@Composable
fun ModelsTab(
    state: ServerState.Snapshot,
    download: ServerState.Download?,
    installed: List<String>,
    onPickFile: () -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var token by remember { mutableStateOf("") }

    Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {

        // Raccomandazione in base alla RAM del dispositivo.
        if (state.ramTotalMb > 0) {
            SectionCard("CONSIGLIATO PER QUESTO TELEFONO") {
                val budget = state.ramTotalMb
                val fit = ModelCatalog.models.filter { it.ramNeededMb <= budget }
                val best = fit.maxByOrNull { it.ramNeededMb }
                Text(
                    if (best != null)
                        "RAM ${budget}MB → fino a ${best.params} (${best.name})"
                    else "RAM ${budget}MB → solo modelli molto piccoli",
                    color = C.Text, fontSize = 13.sp
                )
            }
        }

        // Download in corso.
        if (download != null && !download.done) {
            SectionCard("DOWNLOAD") {
                Text(download.name, color = C.Text, fontWeight = FontWeight.Bold)
                Spacer(Modifier.height(8.dp))
                if (download.error != null) {
                    Text("Errore: ${download.error}", color = C.Danger, fontSize = 12.sp)
                } else {
                    LinearProgressIndicator(
                        progress = download.pct / 100f,
                        modifier = Modifier.fillMaxWidth(),
                        color = C.Accent,
                    )
                    Spacer(Modifier.height(6.dp))
                    Text(
                        "${download.pct}%  ·  ${mb(download.receivedBytes)} / ${mb(download.totalBytes)}",
                        color = C.Muted, fontSize = 12.sp
                    )
                    TextButton(onClick = { ModelDownloader.cancel() }) {
                        Text("Annulla", color = C.Danger)
                    }
                }
            }
        }

        // Token gated.
        SectionCard("CATALOGO") {
            OutlinedTextField(
                value = token,
                onValueChange = { token = it },
                label = { Text("Token Hugging Face (per modelli gated, opzionale)") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth()
            )
            Spacer(Modifier.height(12.dp))
            ModelCatalog.models.forEach { m ->
                CatalogRow(
                    model = m,
                    installed = installed.contains(m.fileName),
                    active = state.modelName == m.fileName,
                    downloading = download != null && !download.done && download.id == m.id,
                    onDownload = {
                        scope.launch(Dispatchers.IO) {
                            runCatching {
                                ModelDownloader.download(context, m, token.ifBlank { null })
                                ModelRepository.refresh(context)
                            }
                        }
                    },
                    onActivate = {
                        scope.launch(Dispatchers.IO) {
                            runCatching {
                                val path = ModelRepository.dir(context).resolve(m.fileName).absolutePath
                                Brain.loadFromPath(context, path, maxTokens = 1024)
                            }
                        }
                    },
                )
                Spacer(Modifier.height(10.dp))
            }
        }

        SectionCard("INSTALLATI") {
            if (installed.isEmpty()) {
                Text("Nessun modello. Scarica dal catalogo o aggiungi da file.", color = C.Muted, fontSize = 12.sp)
            }
            installed.forEach { name ->
                Row(
                    Modifier.fillMaxWidth().padding(vertical = 6.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Column(Modifier.fillMaxWidth(0.55f)) {
                        Text(name, color = C.Text, fontSize = 13.sp)
                        if (state.modelName == name) Text("attivo", color = C.Mint, fontSize = 11.sp)
                    }
                    TextButton(onClick = {
                        scope.launch(Dispatchers.IO) {
                            runCatching {
                                Brain.loadFromPath(
                                    context,
                                    ModelRepository.dir(context).resolve(name).absolutePath, 1024
                                )
                            }
                        }
                    }) { Text("Attiva", color = C.Accent) }
                    TextButton(onClick = { ModelRepository.delete(context, name) }) {
                        Text("Elimina", color = C.Danger)
                    }
                }
            }
        }

        OutlinedButton(onClick = onPickFile, modifier = Modifier.fillMaxWidth()) {
            Text("Aggiungi modello da file…", color = C.Text)
        }
        Spacer(Modifier.height(8.dp))
    }
}

@Composable
private fun CatalogRow(
    model: CatalogModel,
    installed: Boolean,
    active: Boolean,
    downloading: Boolean,
    onDownload: () -> Unit,
    onActivate: () -> Unit,
) {
    Column(Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(model.name, color = C.Text, fontWeight = FontWeight.Bold, fontSize = 14.sp)
            Spacer(Modifier.fillMaxWidth(0.6f))
            if (active) Chip("ATTIVO", C.Mint)
            else if (model.gated) Chip("gated", C.Amber)
        }
        Text(
            "${model.params} · ~${model.approxSizeMb}MB · serve ${model.ramNeededMb}MB RAM",
            color = C.Muted, fontSize = 11.sp
        )
        if (model.note.isNotEmpty()) Text(model.note, color = C.Muted, fontSize = 11.sp)
        Spacer(Modifier.height(6.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            if (installed) {
                Button(
                    onClick = onActivate,
                    enabled = !active,
                    colors = ButtonDefaults.buttonColors(containerColor = C.PanelHi),
                ) { Text(if (active) "In uso" else "Attiva", fontSize = 13.sp) }
            } else {
                Button(
                    onClick = onDownload,
                    enabled = !downloading,
                    colors = ButtonDefaults.buttonColors(containerColor = C.Accent),
                ) { Text(if (downloading) "Scarico…" else "Scarica", fontSize = 13.sp) }
            }
        }
    }
}

private fun mb(bytes: Long): String =
    if (bytes <= 0) "?" else String.format("%.0f MB", bytes / (1024.0 * 1024.0))
