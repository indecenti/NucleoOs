package com.nucleoos.mind.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.ui.draw.clip
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.nucleoos.mind.core.ServerState
import kotlinx.coroutines.delay

@Composable
fun MonitorTab(state: ServerState.Snapshot, history: List<ServerState.ReqLog>) {
    var now by remember { mutableStateOf(android.os.SystemClock.elapsedRealtime()) }
    LaunchedEffect(state.running) {
        while (true) { now = android.os.SystemClock.elapsedRealtime(); delay(1000) }
    }
    val uptime = if (state.running && state.startedAtElapsed > 0)
        (now - state.startedAtElapsed) / 1000 else 0

    Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
        SectionCard("MEMORIA") {
            val used = (state.ramTotalMb - state.ramAvailMb).coerceAtLeast(0)
            val frac = if (state.ramTotalMb > 0) used.toFloat() / state.ramTotalMb else 0f
            Bar(frac, if (frac > 0.85f) C.Danger else C.Accent)
            Spacer(Modifier.height(6.dp))
            KeyVal("Usata", "${used} / ${state.ramTotalMb} MB")
            KeyVal("Disponibile", "${state.ramAvailMb} MB")
        }
        SectionCard("DISPOSITIVO") {
            KeyVal(
                "Batteria",
                if (state.batteryPct >= 0) "${state.batteryPct}%${if (state.charging) " ⚡" else ""}" else "—",
                valueColor = if (state.batteryPct in 0..14 && !state.charging) C.Danger else C.Text
            )
            KeyVal(
                "Temperatura", state.thermal,
                valueColor = if (state.thermal in listOf("severo", "critico")) C.Amber else C.Text
            )
            KeyVal("Core CPU", state.cpuCores.toString())
            if (state.throttled) {
                Spacer(Modifier.height(6.dp))
                Text("⚠ Guard attivo: ${state.throttleReason}", color = C.Amber, fontSize = 12.sp)
            }
        }
        SectionCard("SESSIONE") {
            KeyVal("Uptime", fmt(uptime))
            KeyVal("Richieste servite", state.requestsServed.toString())
            KeyVal("Token generati", state.tokensGenerated.toString())
            KeyVal("Velocità", String.format("%.1f tok/s", state.lastTokensPerSec))
        }

        SectionCard("ULTIME RICHIESTE") {
            if (history.isEmpty()) {
                Text("(nessuna richiesta ancora)", color = C.Muted, fontSize = 12.sp)
            }
            history.forEach { r ->
                Row(Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
                    Text(r.endpoint, color = C.Text, fontSize = 11.sp, modifier = Modifier.fillMaxWidth(0.55f))
                    Text("${r.ms}ms · ${r.tokens}t", color = C.Muted, fontSize = 11.sp)
                }
            }
        }
    }
}

@Composable
private fun Bar(frac: Float, color: Color) {
    Box(
        Modifier.fillMaxWidth().height(10.dp).clip(RoundedCornerShape(50)).background(C.PanelHi)
    ) {
        Box(
            Modifier.fillMaxWidth(frac.coerceIn(0f, 1f)).height(10.dp)
                .clip(RoundedCornerShape(50)).background(color)
        )
    }
}

private fun fmt(s: Long): String {
    val h = s / 3600; val m = (s % 3600) / 60; val sec = s % 60
    return if (h > 0) "%dh %02dm".format(h, m) else "%dm %02ds".format(m, sec)
}
