package com.nucleoos.mind.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.ui.draw.clip
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.nucleoos.mind.core.ServerState
import com.nucleoos.mind.llm.Brain
import com.nucleoos.mind.llm.GenParams
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** Banco di prova locale: parla col modello senza uscire dall'app. */
@Composable
fun PlaygroundTab(state: ServerState.Snapshot) {
    val scope = rememberCoroutineScope()
    var input by remember { mutableStateOf("") }
    var output by remember { mutableStateOf("") }
    var thinking by remember { mutableStateOf(false) }

    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        SectionCard("BANCO DI PROVA") {
            Text(
                "Motore: ${state.engineName} · ${state.modelName}",
                color = C.Muted, fontSize = 12.sp
            )
        }

        Column(
            Modifier.fillMaxWidth().heightCard().clip(RoundedCornerShape(14.dp))
                .background(C.Panel).padding(14.dp)
        ) {
            Text(
                output.ifEmpty { "La risposta del modello apparirà qui." },
                color = if (output.isEmpty()) C.Muted else C.Text, fontSize = 14.sp
            )
        }

        OutlinedTextField(
            value = input,
            onValueChange = { input = it },
            label = { Text("Scrivi un prompt") },
            modifier = Modifier.fillMaxWidth()
        )
        Button(
            onClick = {
                val prompt = input.trim()
                if (prompt.isEmpty() || thinking) return@Button
                output = ""; thinking = true
                scope.launch {
                    withContext(Dispatchers.IO) {
                        runCatching {
                            Brain.engine.generate(
                                Brain.buildPrompt(listOf("user" to prompt)),
                                GenParams(maxTokens = 512)
                            ) { delta -> output += delta }
                        }.onFailure { output = "Errore: ${it.message}" }
                    }
                    thinking = false
                }
            },
            enabled = !thinking,
            colors = ButtonDefaults.buttonColors(containerColor = C.Accent),
            modifier = Modifier.fillMaxWidth()
        ) { Text(if (thinking) "Genero…" else "Invia") }
    }
}

private fun Modifier.heightCard(): Modifier = this.height(260.dp)
