@echo off
REM Apre ANIMA in Chrome con il microfono abilitato sull'origine HTTP del device.
REM Il flag --unsafely-treat-insecure-origin-as-secure non si puo' impostare da una
REM pagina web (sandbox del browser): va passato all'avvio di Chrome. Doppio-clic e basta.
REM Cambia DEVICE se l'IP del Cardputer e' diverso.

set "DEVICE=http://192.168.0.166"

set "CHROME=%ProgramFiles%\Google\Chrome\Application\chrome.exe"
if not exist "%CHROME%" set "CHROME=%ProgramFiles(x86)%\Google\Chrome\Application\chrome.exe"
if not exist "%CHROME%" set "CHROME=%LocalAppData%\Google\Chrome\Application\chrome.exe"
if not exist "%CHROME%" (
  echo Chrome non trovato. Apro col browser di default ^(il microfono potrebbe restare bloccato^).
  start "" "%DEVICE%/apps/anima/"
  exit /b
)

REM --user-data-dir dedicato: cosi' il flag viene applicato in modo affidabile e
REM non tocca il tuo profilo Chrome principale.
start "" "%CHROME%" --unsafely-treat-insecure-origin-as-secure="%DEVICE%" --user-data-dir="%LocalAppData%\AnimaVoice" "%DEVICE%/apps/anima/"
