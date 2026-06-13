# Test Lab — report delle corse

Generati automaticamente dal cockpit ([../test_lab.py](../test_lab.py)) **dopo ogni corsa di test**
(qualsiasi pulsante: ANIMA offline / Esegui tutti (PC) / Solo NL / Selezione / 📡 Test device).
Servono a rileggere l'esito di una corsa **senza rifare i test** — anche in una sessione futura.

## Cosa trovi qui

| File | Cosa |
|---|---|
| `latest.json` | L'ultimo report, sempre aggiornato. **Parti da qui.** |
| `latest.md` | Stesso contenuto in Markdown leggibile (riepilogo + falliti + più lenti). |
| `index.jsonl` | Indice append-only, **una riga per corsa** (ts, scope, file, conteggi). Per sapere qual è l'ultimo e lo storico in un colpo d'occhio. |
| `run-<YYYYMMDD-HHMMSS>.json` | Report versionato completo di quella corsa. |
| `run-<YYYYMMDD-HHMMSS>.md` | Versione Markdown della stessa corsa. |

Si tengono gli ultimi 40 `run-*` (i più vecchi vengono potati). `latest.*` e `index.jsonl` non vengono mai potati.

## Schema di `run-*.json` / `latest.json`

```jsonc
{
  "ts": "2026-06-08T00:45:01",      // ISO, ora locale
  "scope": "full-pc | device | partial",  // full-pc = sweep ≥20 test su PC; device = solo categoria device-load
  "device_ip": "192.168.0.166|null",       // valorizzato solo se la corsa includeva test device
  "counts": { "total": N, "pass": N, "fail": N, "error": N, "skip": N },
  "health": { "hallucinations": 0, "green": [verdi, totali], ... },  // dalle metriche del registry
  "tests": [                          // ogni test della corsa
    { "id", "label", "category", "status", "summary", "duration_s", "cmd" }
  ],
  "failures": [                       // SOLO fail/error, con output troncato (ultimi ~6KB) per diagnosi
    { "id", "label", "category", "summary", "output" }
  ]
}
```

## Per un agente che legge a freddo
- `latest.json` → `counts` e `health` per il verdetto; `failures[].output` per capire **perché** un test è rosso.
- `scope: "device"` + `device_ip` → quella corsa ha colpito il Cardputer reale (gli altri girano su PC).
- `index.jsonl` ultima riga = corsa più recente.
