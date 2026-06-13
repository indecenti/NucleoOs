package com.nucleoos.mind.ui

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.draw.clip
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.nucleoos.mind.core.ServerState

@Composable
fun ServerTab(
    state: ServerState.Snapshot,
    onStart: () -> Unit,
    onStop: () -> Unit,
) {
    Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
        SectionCard {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Chip(
                    if (state.running) "ONLINE" else "OFFLINE",
                    if (state.running) C.Mint else C.Muted
                )
                Spacer(Modifier.fillMaxWidth(0.5f))
                if (state.throttled) Chip("THROTTLED", C.Amber)
                else if (state.busy) Chip("BUSY", C.Accent)
            }
            Spacer(Modifier.height(12.dp))
            KeyVal("Indirizzo", "http://${state.ip}:${state.port}")
            KeyVal("mDNS", "_anima._tcp · NucleoMind")
            KeyVal("Motore", state.engineName)
            KeyVal(
                "Modello", state.modelName,
                valueColor = if (state.modelLoaded) C.Mint else C.Muted
            )
            if (state.throttled) {
                Spacer(Modifier.height(6.dp))
                Text("Guard: ${state.throttleReason}", color = C.Amber, fontSize = 12.sp)
            }
        }

        // QR di pairing.
        if (state.running) {
            val url = "http://${state.ip}:${state.port}"
            val qr = remember(url) { qrBitmap(url) }
            SectionCard("PAIRING") {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    if (qr != null) {
                        Box(
                            Modifier.clip(RoundedCornerShape(8.dp))
                                .background(C.Bg).padding(6.dp)
                        ) {
                            Image(qr, contentDescription = "QR", modifier = Modifier.size(120.dp))
                        }
                    }
                    Spacer(Modifier.fillMaxWidth(0.08f))
                    Column {
                        Text("Inquadra per connettere", color = C.Text, fontWeight = FontWeight.Bold)
                        Spacer(Modifier.height(4.dp))
                        Text(url, color = C.Accent, fontSize = 13.sp)
                        Spacer(Modifier.height(6.dp))
                        Text(
                            "Il Cardputer può anche scoprirlo da solo via mDNS.",
                            color = C.Muted, fontSize = 11.sp
                        )
                    }
                }
            }
        }

        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Button(
                onClick = onStart,
                enabled = !state.running,
                colors = ButtonDefaults.buttonColors(containerColor = C.Accent),
                modifier = Modifier.fillMaxWidth(0.5f)
            ) { Text("Avvia server") }
            OutlinedButton(
                onClick = onStop,
                enabled = state.running,
                modifier = Modifier.fillMaxWidth()
            ) { Text("Ferma", color = C.Text) }
        }

        SectionCard("ENDPOINT") {
            Text("GET  /health", color = C.Muted, fontSize = 12.sp)
            Text("GET  /metrics", color = C.Muted, fontSize = 12.sp)
            Text("GET  /v1/models", color = C.Muted, fontSize = 12.sp)
            Text("POST /v1/chat/completions  (stream)", color = C.Muted, fontSize = 12.sp)
            Text("POST /v1/distill  · teacher → card", color = C.Mint, fontSize = 12.sp)
        }
    }
}
