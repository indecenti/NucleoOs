package com.nucleoos.mind.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.nucleoos.mind.core.ServerState
import com.nucleoos.mind.core.Settings

@Composable
fun SettingsTab() {
    val s by Settings.data.collectAsState()
    val server by ServerState.state.collectAsState()

    Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {

        SectionCard("RETE") {
            OutlinedTextField(
                value = s.port.toString(),
                onValueChange = { v ->
                    val p = v.filter { it.isDigit() }.take(5).toIntOrNull()
                    if (p != null && p in 1024..65535) Settings.update { it.copy(port = p) }
                },
                label = { Text("Porta") },
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                modifier = Modifier.fillMaxWidth()
            )
            if (server.running) {
                Text("La nuova porta si applica al prossimo avvio.", color = C.Amber, fontSize = 11.sp)
            }
        }

        SectionCard("SICUREZZA") {
            OutlinedTextField(
                value = s.apiKey,
                onValueChange = { Settings.update { st -> st.copy(apiKey = it.trim()) } },
                label = { Text("API key (vuoto = aperto in LAN)") },
                singleLine = true,
                visualTransformation = PasswordVisualTransformation(),
                modifier = Modifier.fillMaxWidth()
            )
            Row {
                TextButton(onClick = {
                    val k = "nm-" + (server.startedAtElapsed.toString(36) + server.requestsServed.toString(36))
                        .padEnd(10, '0').take(10)
                    Settings.update { it.copy(apiKey = k) }
                }) { Text("Genera", color = C.Accent) }
                if (s.apiKey.isNotBlank()) {
                    TextButton(onClick = { Settings.update { it.copy(apiKey = "") } }) {
                        Text("Rimuovi", color = C.Danger)
                    }
                }
            }
        }

        SectionCard("AVVIO AUTOMATICO") {
            ToggleRow("All'apertura dell'app", s.autoStartOnLaunch) { v ->
                Settings.update { it.copy(autoStartOnLaunch = v) }
            }
            ToggleRow("Al boot del telefono", s.autoStartOnBoot) { v ->
                Settings.update { it.copy(autoStartOnBoot = v) }
            }
            ToggleRow("Ricarica l'ultimo modello", s.autoReloadModel) { v ->
                Settings.update { it.copy(autoReloadModel = v) }
            }
            if (s.autoStartOnBoot) {
                Text("Su Android 14+ l'avvio al boot può essere limitato dal sistema.",
                    color = C.Muted, fontSize = 11.sp)
            }
        }

        SectionCard("GENERAZIONE") {
            SliderRow("Temperatura", s.temperature, 0f..1.5f) {
                Settings.update { st -> st.copy(temperature = it) }
            }
            SliderRow("Top-K", s.topK.toFloat(), 1f..100f) {
                Settings.update { st -> st.copy(topK = it.toInt()) }
            }
            SliderRow("Max token", s.maxTokens.toFloat(), 64f..2048f) {
                Settings.update { st -> st.copy(maxTokens = it.toInt()) }
            }
        }

        SectionCard("SYSTEM PROMPT") {
            OutlinedTextField(
                value = s.systemPrompt,
                onValueChange = { Settings.update { st -> st.copy(systemPrompt = it) } },
                label = { Text("Istruzioni di sistema (opzionale)") },
                modifier = Modifier.fillMaxWidth().height(110.dp)
            )
        }
        Spacer(Modifier.height(8.dp))
    }
}

@Composable
private fun ToggleRow(label: String, value: Boolean, onChange: (Boolean) -> Unit) {
    Row(
        Modifier.fillMaxWidth().padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(label, color = C.Text, fontSize = 14.sp, modifier = Modifier.fillMaxWidth(0.75f))
        Switch(checked = value, onCheckedChange = onChange)
    }
}

@Composable
private fun SliderRow(label: String, value: Float, range: ClosedFloatingPointRange<Float>, onChange: (Float) -> Unit) {
    Column(Modifier.padding(vertical = 4.dp)) {
        Row {
            Text(label, color = C.Muted, fontSize = 13.sp, modifier = Modifier.fillMaxWidth(0.7f))
            Text(
                if (value < 5f) String.format("%.2f", value) else value.toInt().toString(),
                color = C.Text, fontSize = 13.sp
            )
        }
        Slider(value = value, onValueChange = onChange, valueRange = range)
    }
}
