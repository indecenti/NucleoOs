/* SPDX-License-Identifier: AGPL-3.0-or-later */
// NucleoOS — the pairing dialog, shared by the shell and by any app that hits a
// gated route.
//
// Why this is a module and not shell code: apps run in their own frames, so a
// fetch interceptor in the shell cannot see them. Whoever gets the 401 has to be
// able to ask for the PIN, and every one of them asking in its own words is how
// "pairing_required" turns into "questa app è rotta" in the user's head.
//
// The PIN is not shown in the browser: /api/pair/speak reads it aloud on the
// CONSOLE's speaker. Only someone holding the device hears it — that is the whole
// proof of possession, and it is why the dialog leads with that button.

// on_*: the device is still OPEN and the caller was refused anyway (today: the
// terminal, gated harder because it is a shell). Turning protection on is no
// longer something any browser on the LAN can do — the server used to hand the
// PIN to whoever asked and pair them, which made "open by default" a one-request
// shell. Now it is done from the console itself or over the USB cable, and the
// PIN is read on the console's screen (quick menu › Network) or heard aloud.
const T = {
  it: { title: 'Accoppia questo browser', body: 'Per comandare il dispositivo (file, terminale, impostazioni) questo browser deve essere accoppiato una volta.',
        speak: '🔊 Leggi il PIN sul dispositivo', speaking: 'PIN letto sul dispositivo…',
        pin: 'PIN a 6 cifre', ok: 'Accoppia', cancel: 'Annulla', wrong: 'PIN errato', off: 'Accoppiamento non attivo su questo dispositivo',
        done: 'Accoppiato ✓', err: 'Dispositivo non raggiungibile', file: 'Il PIN è anche sullo schermo della console: menu rapido › Rete.',
        throttled: 'Attendi qualche secondo e riprova.', locked: 'Troppi tentativi: riprova tra un minuto.',
        on_title: 'Serve un PIN', on_body: 'Questa funzione apre una shell sul dispositivo, quindi non basta essere sulla stessa rete: serve la protezione con PIN. Per sicurezza si attiva solo dalla console o da un browser collegato col cavo USB (http://192.168.42.1:8080). Poi il PIN compare sullo schermo della console (menu rapido › Rete) e lo inserisci qui.',
        on_btn: 'Attiva', recheck: 'L\'ho attivata: inserisci il PIN',
        on_local: 'Da questo browser non si può attivare: fallo dalla console o via cavo USB, poi premi di nuovo.',
        on_still: 'La protezione non risulta ancora attiva sul dispositivo.',
        on_done: 'Protezione attiva. PIN: {pin}', on_done_nopin: 'Protezione attiva. Il PIN è sullo schermo della console (menu rapido › Rete): servirà agli altri browser.' },
  en: { title: 'Pair this browser', body: 'To drive the device (files, terminal, settings) this browser has to be paired once.',
        speak: '🔊 Read the PIN out on the device', speaking: 'PIN read out on the device…',
        pin: '6-digit PIN', ok: 'Pair', cancel: 'Cancel', wrong: 'Wrong PIN', off: 'Pairing is not enabled on this device',
        done: 'Paired ✓', err: 'Device unreachable', file: 'The PIN is also on the console screen: quick menu › Network.',
        throttled: 'Wait a few seconds and try again.', locked: 'Too many attempts: try again in a minute.',
        on_title: 'A PIN is required', on_body: 'This feature opens a shell on the device, so being on the same network is not enough: PIN protection is needed. For safety it can only be turned on from the console itself or from a browser plugged in over the USB cable (http://192.168.42.1:8080). The PIN then appears on the console screen (quick menu › Network) and you enter it here.',
        on_btn: 'Turn on', recheck: 'I turned it on: enter the PIN',
        on_local: 'It cannot be turned on from this browser: do it from the console or over the USB cable, then press again.',
        on_still: 'Protection is not on yet on the device.',
        on_done: 'Protection on. PIN: {pin}', on_done_nopin: 'Protection on. The PIN is on the console screen (quick menu › Network): other browsers will need it.' },
  es: { title: 'Emparejar este navegador', body: 'Para controlar el dispositivo (archivos, terminal, ajustes) hay que emparejar este navegador una vez.',
        speak: '🔊 Decir el PIN en el dispositivo', speaking: 'PIN dicho en el dispositivo…',
        pin: 'PIN de 6 cifras', ok: 'Emparejar', cancel: 'Cancelar', wrong: 'PIN incorrecto', off: 'El emparejamiento no está activo en este dispositivo',
        done: 'Emparejado ✓', err: 'Dispositivo inalcanzable', file: 'El PIN también está en la pantalla de la consola: menú rápido › Red.',
        throttled: 'Espera unos segundos y vuelve a intentarlo.', locked: 'Demasiados intentos: vuelve a intentarlo en un minuto.',
        on_title: 'Hace falta un PIN', on_body: 'Esta función abre una shell en el dispositivo, así que estar en la misma red no basta: hace falta la protección por PIN. Por seguridad solo se activa desde la consola o desde un navegador conectado por cable USB (http://192.168.42.1:8080). Después el PIN aparece en la pantalla de la consola (menú rápido › Red) y lo introduces aquí.',
        on_btn: 'Activar', recheck: 'Ya la activé: introducir el PIN',
        on_local: 'No se puede activar desde este navegador: hazlo desde la consola o por cable USB y vuelve a pulsar.',
        on_still: 'La protección aún no está activa en el dispositivo.',
        on_done: 'Protección activa. PIN: {pin}', on_done_nopin: 'Protección activa. El PIN está en la pantalla de la consola (menú rápido › Red): lo necesitarán los demás navegadores.' },
  fr: { title: 'Appairer ce navigateur', body: "Pour piloter l'appareil (fichiers, terminal, réglages), ce navigateur doit être appairé une fois.",
        speak: '🔊 Énoncer le PIN sur l\'appareil', speaking: 'PIN énoncé sur l\'appareil…',
        pin: 'PIN à 6 chiffres', ok: 'Appairer', cancel: 'Annuler', wrong: 'PIN incorrect', off: "L'appairage n'est pas actif sur cet appareil",
        done: 'Appairé ✓', err: 'Appareil injoignable', file: "Le PIN est aussi affiché sur l'écran de la console : menu rapide › Réseau.",
        throttled: 'Patientez quelques secondes et réessayez.', locked: 'Trop de tentatives : réessayez dans une minute.',
        on_title: 'Un PIN est nécessaire', on_body: "Cette fonction ouvre un shell sur l'appareil : être sur le même réseau ne suffit pas, il faut la protection par PIN. Par sécurité, elle ne s'active que depuis la console ou depuis un navigateur relié par câble USB (http://192.168.42.1:8080). Le PIN apparaît ensuite sur l'écran de la console (menu rapide › Réseau) et vous le saisissez ici.",
        on_btn: 'Activer', recheck: "Je l'ai activée : saisir le PIN",
        on_local: "Impossible de l'activer depuis ce navigateur : faites-le depuis la console ou par câble USB, puis appuyez à nouveau.",
        on_still: "La protection n'est pas encore active sur l'appareil.",
        on_done: 'Protection active. PIN : {pin}', on_done_nopin: "Protection active. Le PIN est sur l'écran de la console (menu rapide › Réseau) : les autres navigateurs en auront besoin." },
  de: { title: 'Diesen Browser koppeln', body: 'Um das Gerät zu steuern (Dateien, Terminal, Einstellungen), muss dieser Browser einmal gekoppelt werden.',
        speak: '🔊 PIN auf dem Gerät vorlesen', speaking: 'PIN auf dem Gerät vorgelesen…',
        pin: '6-stellige PIN', ok: 'Koppeln', cancel: 'Abbrechen', wrong: 'Falsche PIN', off: 'Kopplung ist auf diesem Gerät nicht aktiv',
        done: 'Gekoppelt ✓', err: 'Gerät nicht erreichbar', file: 'Die PIN steht auch auf dem Bildschirm der Konsole: Schnellmenü › Netzwerk.',
        throttled: 'Warte ein paar Sekunden und versuche es erneut.', locked: 'Zu viele Versuche: versuche es in einer Minute erneut.',
        on_title: 'Eine PIN ist nötig', on_body: 'Diese Funktion öffnet eine Shell auf dem Gerät — im selben Netz zu sein genügt nicht, es braucht den PIN-Schutz. Aus Sicherheitsgründen lässt er sich nur an der Konsole selbst oder in einem per USB-Kabel verbundenen Browser einschalten (http://192.168.42.1:8080). Danach erscheint die PIN auf dem Bildschirm der Konsole (Schnellmenü › Netzwerk) und du gibst sie hier ein.',
        on_btn: 'Einschalten', recheck: 'Ist eingeschaltet: PIN eingeben',
        on_local: 'Von diesem Browser aus geht das nicht: schalte es an der Konsole oder per USB-Kabel ein und tippe dann erneut.',
        on_still: 'Der Schutz ist auf dem Gerät noch nicht aktiv.',
        on_done: 'Schutz aktiv. PIN: {pin}', on_done_nopin: 'Schutz aktiv. Die PIN steht auf dem Bildschirm der Konsole (Schnellmenü › Netzwerk): andere Browser brauchen sie.' },
};
// The OS language is a DEVICE fact (/api/lang), not a per-browser one — the same
// console answers on several origins. The document/browser hint is only the seed
// used until the device has answered.
let LANG = (document.documentElement.lang || navigator.language || 'it').slice(0, 2);
async function syncLang() {
  try {
    const r = await fetch('/api/lang', { cache: 'no-store' });
    const j = await r.json();
    if (j && T[j.lang]) LANG = j.lang;
  } catch {}
}
const t = (k) => (T[LANG] || T.it)[k] || T.it[k] || k;

const CSS = `
.ncpair-ovl{position:fixed;inset:0;z-index:2147483000;display:flex;align-items:center;justify-content:center;
  background:rgba(4,8,20,.72);backdrop-filter:blur(3px);font:13px/1.5 system-ui,Segoe UI,sans-serif}
.ncpair-box{width:min(92vw,380px);background:var(--panel,#18233a);color:var(--text,#e8eefc);
  border:1px solid var(--line,#2a3a5e);border-radius:14px;padding:18px;box-shadow:0 18px 50px rgba(0,0,0,.5)}
.ncpair-box h2{margin:0 0 6px;font-size:16px}
.ncpair-box p{margin:0 0 12px;color:var(--muted,#8aa0c8);font-size:12px}
.ncpair-box button{font:inherit;border-radius:9px;padding:8px 12px;cursor:pointer;border:1px solid var(--line,#2a3a5e);
  background:#80808022;color:var(--text,#e8eefc)}
.ncpair-box button:hover{background:#80808036}
.ncpair-box button.pri{background:var(--accent,#4ea1ff);border-color:transparent;color:#04101f;font-weight:600}
.ncpair-speak{width:100%;margin-bottom:10px}
.ncpair-box input{width:100%;font:600 20px/1 ui-monospace,monospace;letter-spacing:.32em;text-align:center;
  padding:11px;border-radius:9px;border:1px solid var(--line,#2a3a5e);background:var(--field,#0b1220);
  color:var(--text,#e8eefc);outline:none}
.ncpair-row{display:flex;gap:8px;margin-top:12px}
.ncpair-row .grow{flex:1}
.ncpair-msg{min-height:16px;margin-top:8px;font-size:12px}
.ncpair-msg.err{color:var(--err,#e2576b)} .ncpair-msg.ok{color:var(--ok,#2fd07a)}
.ncpair-foot{margin-top:10px;font-size:11px;color:var(--muted,#8aa0c8)}`;

let cssDone = false;
function injectCss() {
  if (cssDone) return;
  cssDone = true;
  const s = document.createElement('style'); s.textContent = CSS; document.head.appendChild(s);
}

/** {paired, auth:'open'|'pin'} — a fact about the DEVICE, never cached in localStorage. */
export async function pairStatus() {
  try {
    const r = await fetch('/api/auth/status', { cache: 'no-store' });
    return r.ok ? await r.json() : null;
  } catch { return null; }
}

let openDialog = null;   // one at a time, shared by every concurrent caller

/** Show the pairing dialog. Resolves true once this browser may drive the device,
 *  false if dismissed.
 *
 *  Two shapes, because there are two situations. If the device already asks for a
 *  PIN, this collects it. If the device is still open and the caller nonetheless
 *  got refused — which today means the terminal, gated harder than everything else
 *  because it is a shell — the dialog says how to turn protection ON. It used to do
 *  it right here and pair this browser in the same step; that was the hole (any
 *  LAN client could get the PIN that way), so now only the console or the USB
 *  cable can switch it on, and a WiFi browser is told where, then re-checks. */
export function pairDialog() {
  if (openDialog) return openDialog;
  injectCss();
  openDialog = (async () => {
    const [st] = await Promise.all([pairStatus(), syncLang()]);
    return new Promise((resolve) => {
      const ovl = document.createElement('div');
      ovl.className = 'ncpair-ovl';
      ovl.innerHTML =
        '<div class="ncpair-box" role="dialog" aria-modal="true">' +
        '<h2></h2><p></p>' +
        '<button class="ncpair-speak"></button>' +
        '<input inputmode="numeric" autocomplete="off" maxlength="6" placeholder="······">' +
        '<div class="ncpair-msg"></div>' +
        '<div class="ncpair-row"><button class="ncpair-cancel"></button><div class="grow"></div><button class="pri ncpair-ok"></button></div>' +
        '<div class="ncpair-foot"></div></div>';
      const q = (s) => ovl.querySelector(s);
      const msg = (text, cls) => { const m = q('.ncpair-msg'); m.textContent = text; m.className = 'ncpair-msg ' + (cls || ''); };
      const done = (v) => { try { ovl.remove(); } catch {} openDialog = null; resolve(v); };
      // Each mode replaces the OK button's handler: onclick, not addEventListener,
      // so switching from "turn on" to "enter the PIN" cannot leave both wired.
      const ok = q('.ncpair-ok');

      q('.ncpair-cancel').textContent = t('cancel');
      q('.ncpair-cancel').onclick = () => done(false);

      // PIN entry — the normal case once the device asks for a PIN.
      const pinMode = () => {
        q('h2').textContent = t('title'); q('p').textContent = t('body');
        q('.ncpair-speak').style.display = '';
        q('input').style.display = '';
        q('.ncpair-speak').textContent = t('speak');
        q('input').setAttribute('aria-label', t('pin'));
        ok.textContent = t('ok'); ok.style.display = ''; ok.disabled = false;
        q('.ncpair-foot').textContent = t('file');
        msg('');
        q('.ncpair-speak').onclick = async () => {
          msg(t('speaking'));
          try {
            const r = await fetch('/api/pair/speak', { method: 'POST' });
            const j = await r.json().catch(() => ({}));
            if (j.error === 'throttled') msg(t('throttled'), 'err');
            else if (j.ok === false) msg(t('off'), 'err');
          } catch { msg(t('err'), 'err'); }
        };
        const submit = async () => {
          const pin = q('input').value.trim();
          if (!pin) return;
          ok.disabled = true;
          try {
            const r = await fetch('/api/pair', { method: 'POST', body: JSON.stringify({ pin }) });
            const j = await r.json().catch(() => ({}));
            if (j.paired) { msg(t('done'), 'ok'); setTimeout(() => done(true), 450); return; }
            msg(r.status === 429 || j.locked ? t('locked') : t('wrong'), 'err');
          } catch { msg(t('err'), 'err'); }
          ok.disabled = false;
          q('input').select();
        };
        ok.onclick = submit;
        q('input').onkeydown = (e) => { if (e.key === 'Enter') submit(); };
        q('input').focus();
      };

      // The device is still open. From the console or the USB cable ("physical")
      // the button turns protection on; from anywhere else the server refuses,
      // so the dialog explains where to do it and then re-checks the device.
      const enableMode = () => {
        q('h2').textContent = t('on_title'); q('p').textContent = t('on_body');
        q('.ncpair-speak').style.display = 'none';
        q('input').style.display = 'none';
        q('.ncpair-foot').textContent = '';
        const physical = !!(st && st.physical);
        ok.textContent = physical ? t('on_btn') : t('recheck');
        ok.onclick = async () => {
          ok.disabled = true;
          try {
            if (!physical) {
              // Someone turned it on at the console? Then this is just a PIN prompt.
              const now = await pairStatus();
              if (now && now.auth === 'pin') { if (now.paired) { done(true); return; } pinMode(); return; }
              msg(t('on_still'), 'err');
            } else {
              const r = await fetch('/api/pair/mode', { method: 'POST', body: JSON.stringify({ enabled: true }) });
              const j = await r.json().catch(() => ({}));
              if (j.enabled) {
                // A physical browser is trusted without a PIN. The PIN only comes
                // back to the console itself (loopback); everyone else reads it
                // on its screen.
                msg(j.pin ? t('on_done').replace('{pin}', j.pin) : t('on_done_nopin'), 'ok');
                ok.style.display = 'none';
                q('.ncpair-cancel').textContent = t('ok');
                q('.ncpair-cancel').onclick = () => done(true);
                return;
              }
              msg(r.status === 401 ? t('on_local') : (j.error || t('err')), 'err');
            }
          } catch { msg(t('err'), 'err'); }
          ok.disabled = false;
        };
        ok.focus();
      };

      document.body.appendChild(ovl);
      if (st && st.auth === 'open') enableMode(); else pinMode();
    });
  })();
  return openDialog;
}

/** True if this browser may drive the device — asking for the PIN if it may not. */
export async function ensurePaired() {
  const st = await pairStatus();
  if (!st) return false;
  if (st.paired) return true;
  return pairDialog();
}

/** fetch() that answers a `pairing_required` 401 by asking for the PIN and
 *  retrying ONCE. Anything else is passed through untouched — a route that fails
 *  for its own reasons must keep failing visibly. */
export async function pairedFetch(input, init) {
  let r = await fetch(input, init);
  if (r.status !== 401) return r;
  let why = '';
  try { why = (await r.clone().json()).error || ''; } catch {}
  if (why !== 'pairing_required') return r;
  if (!(await pairDialog())) return r;
  return fetch(input, init);
}

export default { pairStatus, pairDialog, ensurePaired, pairedFetch };
