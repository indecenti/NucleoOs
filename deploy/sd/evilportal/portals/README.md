# Evil Portal — template (cloni captive portal)

Solo per **test autorizzati** (reti tue, audit con permesso scritto, CTF, security awareness).

Ogni `*.html` qui dentro compare nella lista template dell'app Evil Portal sul Cardputer.
Percorso sul device: `/sd/evilportal/portals/`.

## Regole
- **Tutto inline**: niente CSS/JS/immagini esterni — un client captive non ha internet, quindi
  asset remoti non caricano. Metti stile e immagini (data URI) dentro l'HTML.
- Il form deve fare `method="POST" action="/login"` (oppure GET con query): qualunque campo
  inviato viene salvato in `/sd/evilportal/loot/`.
- I nomi dei campi guidano l'etichettatura: chi contiene `pass/pwd/pin` = password; `user/email/
  account/tel/...` = utente. Tutto il resto è comunque salvato grezzo.
- `{{SSID}}` viene sostituito a runtime col SSID realmente trasmesso.

## Aggiungere un clone
Copia un `.html` qui e risincronizza la SD (`tools/sd-sync.ps1`). Per cloni brand-accurate,
salva la pagina reale e rendila self-contained (asset inline).

Le catture finiscono in `/sd/evilportal/loot/` (una CSV per sessione + `all-credentials.csv`).
