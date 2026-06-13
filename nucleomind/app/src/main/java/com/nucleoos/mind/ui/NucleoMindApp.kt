package com.nucleoos.mind.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.ui.draw.clip
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.nucleoos.mind.core.ServerState

private enum class Tab(val label: String) {
    SERVER("Server"), MODELS("Modelli"), MONITOR("Monitor"), PLAY("Prova"), SETTINGS("Opzioni")
}

@Composable
fun NucleoMindApp(
    onStart: () -> Unit,
    onStop: () -> Unit,
    onPickModel: () -> Unit,
) {
    val scheme = darkColorScheme(
        primary = C.Accent, background = C.Bg, surface = C.Panel, onBackground = C.Text
    )
    MaterialTheme(colorScheme = scheme) {
        val state by ServerState.state.collectAsState()
        val logs by ServerState.logs.collectAsState()
        val download by ServerState.download.collectAsState()
        val installed by ServerState.installed.collectAsState()
        val history by ServerState.history.collectAsState()
        var tab by remember { mutableStateOf(Tab.SERVER) }

        Column(Modifier.fillMaxSize().background(C.Bg)) {
            // Header.
            Column(Modifier.padding(start = 20.dp, end = 20.dp, top = 18.dp, bottom = 6.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("NucleoMind", color = C.Text, fontSize = 26.sp, fontWeight = FontWeight.Bold)
                    Spacer(Modifier.weight(1f))
                    Chip(if (state.running) "● ${state.ip}" else "○ offline",
                        if (state.running) C.Mint else C.Muted)
                }
                Text("Il tuo Ollama per Android", color = C.Muted, fontSize = 12.sp)
            }

            // Contenuto scrollabile.
            Column(
                Modifier.weight(1f).verticalScroll(rememberScrollState())
                    .padding(horizontal = 20.dp)
            ) {
                Spacer(Modifier.height(8.dp))
                when (tab) {
                    Tab.SERVER -> {
                        ServerTab(state, onStart, onStop)
                        Spacer(Modifier.height(14.dp))
                        LogCard(logs)
                    }
                    Tab.MODELS -> ModelsTab(state, download, installed, onPickModel)
                    Tab.MONITOR -> MonitorTab(state, history)
                    Tab.PLAY -> PlaygroundTab(state)
                    Tab.SETTINGS -> SettingsTab()
                }
                Spacer(Modifier.height(20.dp))
            }

            // Bottom nav.
            Row(
                Modifier.fillMaxWidth().background(C.Panel).padding(vertical = 6.dp),
                horizontalArrangement = Arrangement.SpaceEvenly
            ) {
                Tab.entries.forEach { t ->
                    val active = t == tab
                    Column(
                        Modifier.clip(RoundedCornerShape(10.dp))
                            .clickable { tab = t }
                            .padding(horizontal = 9.dp, vertical = 8.dp),
                        horizontalAlignment = Alignment.CenterHorizontally
                    ) {
                        Text(
                            t.label,
                            color = if (active) C.Accent else C.Muted,
                            fontWeight = if (active) FontWeight.Bold else FontWeight.Normal,
                            fontSize = 12.sp
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun LogCard(logs: List<String>) {
    SectionCard("LOG") {
        Box(Modifier.fillMaxWidth().height(180.dp)) {
            Column(Modifier.verticalScroll(rememberScrollState())) {
                if (logs.isEmpty()) Text("(nessun evento)", color = C.Muted, fontSize = 11.sp)
                logs.takeLast(80).forEach {
                    Text(it, color = C.Muted, fontSize = 11.sp, fontFamily = FontFamily.Monospace)
                }
            }
        }
    }
}
