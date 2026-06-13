#!/usr/bin/env python3
"""NucleoOS Flasher — GUI per build, flash, deploy SD e OTA del Cardputer.

Tutto ruota attorno agli script gia' presenti nel repo, cosi' la GUI e gli script
da riga di comando restano allineati:
  - Flash USB : tools/flash.ps1   (ESP-IDF build + idf.py flash)
  - SD Deploy : tools/deploy.ps1  (sync incrementale hash-based, guardia Removable)
  - OTA Wi-Fi : tools/ota.ps1     (push firmware via /api/ota con pairing PIN)
  - Identify  : esptool chip-id   (capire QUALE board e DOVE)

Avvio:  python tools/flasher.py    (o doppio click su tools/flasher.bat)
Dipendenze: pyserial (gia' installato). tkinter e' nella stdlib.
"""

import os
import sys
import re
import json
import queue
import shutil
import threading
import subprocess

import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox

try:
    from serial.tools import list_ports
except Exception:
    list_ports = None

# --- Percorsi -----------------------------------------------------------------
TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(TOOLS_DIR)
FLASH_PS1 = os.path.join(TOOLS_DIR, "flash.ps1")
DEPLOY_PS1 = os.path.join(TOOLS_DIR, "deploy.ps1")
OTA_PS1 = os.path.join(TOOLS_DIR, "ota.ps1")
FIRMWARE = os.path.join(REPO, "firmware")
FW_BIN = os.path.join(FIRMWARE, "build", "nucleoos.bin")
IDF_EXPORT = r"C:\esp\esp-idf\export.ps1"
SETTINGS = os.path.join(TOOLS_DIR, ".flasher.json")

# Dimensione partizione app (factory) da partitions.csv: 0x180000 = 1.5 MB.
APP_PART_SIZE = 0x180000

# Indizi nelle descrizioni porta che suggeriscono un ESP collegato.
ESP_HINTS = ("usb", "serial", "cp210", "ch340", "ch910", "jtag", "uart", "esp")

# PowerShell base senza profilo (avvio piu' rapido e prevedibile).
PS = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass"]


def find_esptool():
    exe = shutil.which("esptool") or shutil.which("esptool.py")
    return [exe] if exe else [sys.executable, "-m", "esptool"]


def load_settings():
    try:
        with open(SETTINGS, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def save_settings(d):
    try:
        with open(SETTINGS, "w", encoding="utf-8") as f:
            json.dump(d, f, indent=2)
    except Exception:
        pass


def human(n):
    n = float(n)
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.2f} {unit}"
        n /= 1024


class Tooltip:
    """Tooltip giallo-pallido con testo a capo, compare dopo un breve ritardo."""

    _open = None  # tooltip attualmente visibile (uno alla volta)

    def __init__(self, widget, text, delay=450, wrap=360):
        self.widget = widget
        self.text = text
        self.delay = delay
        self.wrap = wrap
        self.tip = None
        self._after = None
        widget.bind("<Enter>", self._schedule, add="+")
        widget.bind("<Leave>", self._hide, add="+")
        widget.bind("<ButtonPress>", self._hide, add="+")

    def _schedule(self, _=None):
        self._cancel()
        self._after = self.widget.after(self.delay, self._show)

    def _cancel(self):
        if self._after:
            self.widget.after_cancel(self._after)
            self._after = None

    def _show(self):
        if self.tip or not self.text:
            return
        if Tooltip._open is not None:
            try:
                Tooltip._open.destroy()
            except Exception:
                pass
        x = self.widget.winfo_rootx() + 16
        y = self.widget.winfo_rooty() + self.widget.winfo_height() + 6
        self.tip = tw = tk.Toplevel(self.widget)
        tw.wm_overrideredirect(True)
        tw.wm_geometry(f"+{x}+{y}")
        try:
            tw.wm_attributes("-topmost", True)
        except Exception:
            pass
        lbl = tk.Label(tw, text=self.text, justify="left", background="#fffbe6",
                       foreground="#222", relief="solid", borderwidth=1,
                       wraplength=self.wrap, font=("Segoe UI", 9), padx=8, pady=5)
        lbl.pack()
        Tooltip._open = tw

    def _hide(self, _=None):
        self._cancel()
        if self.tip:
            if Tooltip._open is self.tip:
                Tooltip._open = None
            self.tip.destroy()
            self.tip = None


def tip(widget, text):
    """Scorciatoia: collega un tooltip a un widget e lo ritorna."""
    Tooltip(widget, text)
    return widget


class FlasherApp:
    def __init__(self, root):
        self.root = root
        root.title("NucleoOS Flasher — M5Stack Cardputer")
        root.geometry("860x680")
        root.minsize(760, 560)

        self.q = queue.Queue()
        self.busy = False
        self.proc = None
        self.cfg = load_settings()

        self._build_ui()
        self.refresh_ports()
        self.refresh_drives()
        self.update_fw_info()
        self.root.after(80, self._drain_queue)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    # ========================================================================
    #  UI
    # ========================================================================
    def _build_ui(self):
        style = ttk.Style()
        try:
            style.theme_use("vista")
        except Exception:
            pass

        # --- Riga porta + identificazione (condivisa, in alto) ---------------
        bar = ttk.LabelFrame(self.root, text="Dispositivo")
        bar.pack(fill="x", padx=8, pady=(8, 4))

        row = ttk.Frame(bar); row.pack(fill="x", padx=6, pady=6)
        ttk.Label(row, text="Porta:").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_cb = ttk.Combobox(row, textvariable=self.port_var, width=40, state="readonly")
        self.port_cb.pack(side="left", padx=4)
        tip(self.port_cb, "Porta seriale (COM) a cui è collegato il Cardputer via USB-C.\n"
                          "★ = porta che sembra un ESP. Sul tuo PC è di solito COM3.\n"
                          "Se non sai quale, premi «Identifica board». La scelta viene ricordata.")
        b_ref = ttk.Button(row, text="↻", width=3, command=self.refresh_ports); b_ref.pack(side="left")
        tip(b_ref, "Riscansiona le porte COM. Premilo se hai appena collegato il Cardputer\n"
                   "o se la porta è sparita (succede quando il device cambia modo Wi-Fi/USB).")
        b_det = ttk.Button(row, text="🔍 Identifica board", command=self.detect_chip); b_det.pack(side="left", padx=6)
        tip(b_det, "Interroga la board sulla porta scelta con «esptool chip-id» e mostra\n"
                   "tipo di chip e MAC. Conferma che sia una ESP32-S3 (il Cardputer lo è)\n"
                   "PRIMA di flashare. Non modifica nulla sul device.")

        self.chip_var = tk.StringVar(value="Chip: premi «Identifica board»")
        lbl_chip = ttk.Label(bar, textvariable=self.chip_var, foreground="#067")
        lbl_chip.pack(anchor="w", padx=10, pady=(0, 6))
        tip(lbl_chip, "Risultato dell'identificazione: tipo di chip e indirizzo MAC.\n"
                      "Diventa un avviso rosso se la board NON è una ESP32-S3.")

        # --- Schede ----------------------------------------------------------
        self.nb = nb = ttk.Notebook(self.root)
        nb.pack(fill="x", padx=8, pady=4)
        self._tab_usb(nb)
        self._tab_sd(nb)
        self._tab_ota(nb)
        nb.bind("<<NotebookTabChanged>>", self._on_tab_changed)

        # --- Stato + progress -----------------------------------------------
        self.pb = ttk.Progressbar(self.root, mode="indeterminate")
        self.pb.pack(fill="x", padx=12, pady=(6, 0))
        sb = ttk.Frame(self.root); sb.pack(fill="x", padx=12)
        self.status_var = tk.StringVar(value="Pronto.")
        lbl_st = ttk.Label(sb, textvariable=self.status_var); lbl_st.pack(side="left")
        tip(lbl_st, "Stato dell'operazione corrente (in corso / completata / fallita con exit code).")
        self.fw_var = tk.StringVar(value="")
        lbl_fw = ttk.Label(sb, textvariable=self.fw_var, foreground="#666"); lbl_fw.pack(side="right")
        tip(lbl_fw, "Dimensione del firmware compilato (firmware/build/nucleoos.bin) e quanto\n"
                    "occupa della partizione app da 1.5 MB. Se supera il 100% non entra in flash.")
        self.btn_stop = ttk.Button(sb, text="✖ Stop", command=self.stop_proc, state="disabled")
        self.btn_stop.pack(side="right", padx=8)
        tip(self.btn_stop, "Termina l'operazione in corso (build/flash/deploy/OTA).\n"
                           "Attivo solo mentre qualcosa è in esecuzione.")

        # --- Log -------------------------------------------------------------
        logframe = ttk.Frame(self.root); logframe.pack(fill="both", expand=True, padx=8, pady=6)
        self.log = scrolledtext.ScrolledText(logframe, height=18, bg="#0e0e12", fg="#d4d4d4",
                                             insertbackground="#d4d4d4", font=("Consolas", 9),
                                             wrap="word")
        self.log.pack(fill="both", expand=True)
        self.log.tag_config("err", foreground="#ff6b6b")
        self.log.tag_config("ok", foreground="#5fd75f")
        self.log.tag_config("info", foreground="#5fc7ff")
        self.log.tag_config("cmd", foreground="#c792ea")
        tip(self.log, "Output in tempo reale dei comandi. Viola = comando lanciato,\n"
                      "rosso = errore, verde = successo, azzurro = info.")
        b_clr = ttk.Button(logframe, text="Pulisci log", command=lambda: self.log.delete("1.0", "end"))
        b_clr.pack(anchor="e", pady=(4, 0))
        tip(b_clr, "Svuota la finestra di log. Non interrompe nessuna operazione.")

    def _tab_usb(self, nb):
        t = ttk.Frame(nb); nb.add(t, text="  Flash USB  ")
        self.erase_var = tk.BooleanVar(value=False)
        cb = ttk.Checkbutton(t, text="Erase flash prima del flash (cancella SD interna NVS/config)",
                             variable=self.erase_var)
        cb.pack(anchor="w", padx=10, pady=(10, 4))
        tip(cb, "Se attivo, prima del Build & Flash cancella TUTTA la flash interna.\n"
                "Utile per partire puliti, ma perdi Wi-Fi salvato e setup (NVS).\n"
                "Lascialo spento per i flash normali.")
        b = ttk.Frame(t); b.pack(fill="x", padx=10, pady=8)
        self.btn_build = ttk.Button(b, text="🔨 Solo Build", command=self.do_build)
        self.btn_build.pack(side="left", padx=3)
        tip(self.btn_build, "Compila il firmware SENZA flashare (flash.ps1 -BuildOnly).\n"
                            "Usalo per controllare che compili o per preparare il .bin da inviare via OTA.\n"
                            "Il primo build è lento (set-target esp32s3), i successivi incrementali.")
        self.btn_flash = ttk.Button(b, text="⚡ Build & Flash", command=self.do_build_flash)
        self.btn_flash.pack(side="left", padx=3)
        tip(self.btn_flash, "Compila e scrive il firmware sul Cardputer via USB (flash.ps1 -Port).\n"
                            "È l'operazione principale. Il reset in download mode è automatico:\n"
                            "non devi toccare i tasti. Se la spunta «Erase» è attiva, cancella prima.")
        self.btn_erase = ttk.Button(b, text="🧹 Erase", command=self.do_erase)
        self.btn_erase.pack(side="left", padx=3)
        tip(self.btn_erase, "Cancella SOLO (esptool erase-flash), senza riflashare.\n"
                            "Sparisce anche la config interna (Wi-Fi, setup). Chiede conferma.")
        self.btn_monitor = ttk.Button(b, text="📺 Monitor seriale", command=self.open_monitor)
        self.btn_monitor.pack(side="left", padx=3)
        tip(self.btn_monitor, "Apre «idf.py monitor» in una finestra console separata per vedere i\n"
                              "log di boot del device in tempo reale. Esci dal monitor con Ctrl+].\n"
                              "Tieni il monitor CHIUSO quando flashi (occupa la porta).")
        ttk.Label(t, foreground="#888", wraplength=780, justify="left",
                  text="Build & Flash usa tools/flash.ps1 (ESP-IDF). Il primo build fa set-target "
                       "esp32s3 (lento); i successivi sono incrementali. Il reset in download mode "
                       "e' automatico: non serve toccare i tasti del Cardputer.").pack(anchor="w", padx=10)

    def _tab_sd(self, nb):
        t = ttk.Frame(nb); nb.add(t, text="  SD Deploy  ")
        row = ttk.Frame(t); row.pack(fill="x", padx=10, pady=(10, 4))
        ttk.Label(row, text="Unita' SD:").pack(side="left")
        self.drive_var = tk.StringVar()
        self.drive_cb = ttk.Combobox(row, textvariable=self.drive_var, width=46, state="readonly")
        self.drive_cb.pack(side="left", padx=4)
        tip(self.drive_cb, "La microSD del Cardputer, vista dal lettore del PC. Mostra SOLO unità\n"
                           "rimovibili (per sicurezza): dischi fissi/di sistema non compaiono.\n"
                           "Se la SD non c'è, inseriscila e premi ↻.")
        b_dr = ttk.Button(row, text="↻", width=3, command=self.refresh_drives); b_dr.pack(side="left")
        tip(b_dr, "Riscansiona le unità rimovibili. Premilo dopo aver inserito la microSD.")

        self.dryrun_var = tk.BooleanVar(value=False)
        cb = ttk.Checkbutton(t, text="Dry-run (anteprima, nessuna scrittura)", variable=self.dryrun_var)
        cb.pack(anchor="w", padx=10, pady=4)
        tip(cb, "Simula l'operazione e stampa cosa COPIEREBBE/RIMUOVEREBBE senza\n"
                "scrivere niente. Ottimo per controllare prima di toccare la SD.")
        b = ttk.Frame(t); b.pack(fill="x", padx=10, pady=8)
        self.btn_stage = ttk.Button(b, text="📦 Stage (solo deploy/sd)", command=self.do_stage)
        self.btn_stage.pack(side="left", padx=3)
        tip(self.btn_stage, "Assembla il contenuto OS (registry, app, shell, media, exe) nella\n"
                            "cartella locale deploy/sd del repo, SENZA toccare la SD. Sync incrementale.")
        self.btn_deploy = ttk.Button(b, text="💾 Deploy su SD", command=self.do_deploy)
        self.btn_deploy.pack(side="left", padx=3)
        tip(self.btn_deploy, "Sincronizza deploy/sd → microSD selezionata (deploy.ps1 -To).\n"
                             "Copia solo i file cambiati e rimuove quelli estranei (mirror).\n"
                             "Chiede conferma; rifiuta unità non rimovibili. Usa Dry-run per provare.")
        ttk.Label(t, foreground="#888", wraplength=780, justify="left",
                  text="deploy.ps1 fa sync incrementale (SHA-256): copia solo i file cambiati, "
                       "scrittura atomica, verifica e mirror-delete. La scrittura su SD e' protetta: "
                       "rifiuta qualsiasi unita' non Removable o di sistema/boot.").pack(anchor="w", padx=10)

    def _tab_ota(self, nb):
        t = self.ota_tab = ttk.Frame(nb); nb.add(t, text="  OTA Wi-Fi  ")
        g = ttk.Frame(t); g.pack(fill="x", padx=10, pady=(10, 4))
        ttk.Label(g, text="Host/IP:").grid(row=0, column=0, sticky="w", pady=3)
        self.host_var = tk.StringVar(value=self.cfg.get("host", "nucleo-01.local"))
        e_host = ttk.Entry(g, textvariable=self.host_var, width=30)
        e_host.grid(row=0, column=1, sticky="w", padx=6)
        tip(e_host, "Indirizzo del Cardputer sulla rete: nome mDNS (es. nucleo-01.local) o IP\n"
                    "(es. 192.168.0.166). L'IP lo vedi sullo schermo del device o nel tray della shell.\n"
                    "Se .local non risolve, usa l'IP numerico. Viene ricordato.")
        b_chk = ttk.Button(g, text="🔌 Verifica online", command=self.check_online)
        b_chk.grid(row=0, column=2, sticky="w", padx=6)
        tip(b_chk, "Fa un GET http://host/api/status per vedere se il device risponde,\n"
                   "PRIMA di tentare l'OTA. Non modifica nulla. Mostra anche modo Wi-Fi/SSID/IP.")
        self.online_var = tk.StringVar(value="● sconosciuto")
        self.lbl_online = ttk.Label(g, textvariable=self.online_var, foreground="#999")
        self.lbl_online.grid(row=0, column=3, sticky="w", padx=6)
        tip(self.lbl_online, "Stato raggiungibilità del device: verde = risponde, rosso = non raggiungibile.")

        ttk.Label(g, text="PIN pairing:").grid(row=1, column=0, sticky="w", pady=3)
        self.pin_var = tk.StringVar()
        e_pin = ttk.Entry(g, textvariable=self.pin_var, width=12)
        e_pin.grid(row=1, column=1, sticky="w", padx=6)
        tip(e_pin, "Codice a 6 cifre per autorizzare l'OTA (richiesto dal firmware con pairing).\n"
                   "Leggilo sullo schermo del Cardputer: app Connection → Pair.\n"
                   "Lascia vuoto se il tuo firmware non richiede pairing. Non viene salvato.")
        ttk.Label(g, foreground="#888", text="(6 cifre mostrate sul Cardputer → app Connection → Pair)").grid(row=1, column=2, columnspan=2, sticky="w")
        b = ttk.Frame(t); b.pack(fill="x", padx=10, pady=8)
        self.btn_ota = ttk.Button(b, text="📡 Push OTA", command=self.do_ota)
        self.btn_ota.pack(side="left", padx=3)
        tip(self.btn_ota, "Invia firmware/build/nucleoos.bin al device via Wi-Fi (ota.ps1).\n"
                          "Il device lo scrive nella slot OTA libera, verifica, imposta boot e si riavvia.\n"
                          "Compila prima (Build) se hai cambiato il firmware. Niente USB necessario.")
        ttk.Label(t, foreground="#888", wraplength=780, justify="left",
                  text="Invia firmware/build/nucleoos.bin alla slot OTA libera del device via Wi-Fi "
                       "(serve essere sulla stessa rete). Il device verifica, imposta boot e si riavvia. "
                       "Fai prima un build se hai cambiato il firmware.").pack(anchor="w", padx=10)

        self.action_btns = [self.btn_build, self.btn_flash, self.btn_erase,
                            self.btn_stage, self.btn_deploy, self.btn_ota]

    # ========================================================================
    #  Porte / Drive / Info firmware
    # ========================================================================
    def refresh_ports(self):
        ports = []
        if list_ports:
            for p in list_ports.comports():
                desc = (p.description or "").lower()
                star = "★ " if any(h in desc for h in ESP_HINTS) else "  "
                ports.append((p.device, f"{star}{p.device} — {p.description}"))
        if not ports:
            try:
                out = subprocess.check_output(
                    PS + ["-Command", "[System.IO.Ports.SerialPort]::GetPortNames()"], text=True)
                ports = [(l.strip(), l.strip()) for l in out.split() if l.strip()]
            except Exception:
                pass
        self._port_map = {label: dev for dev, label in ports}
        labels = list(self._port_map.keys())
        self.port_cb["values"] = labels
        saved = self.cfg.get("port")
        pick = next((l for l in labels if self._port_map[l] == saved), None) \
            or next((l for l in labels if l.startswith("★")), labels[0] if labels else "")
        self.port_var.set(pick)
        self.log_line(f"Porte: {len(labels)} trovate." + ("" if labels else " NESSUNA porta!"), "info")

    def selected_port(self):
        return self._port_map.get(self.port_var.get(), "")

    def refresh_drives(self):
        """Elenca SOLO le unita' removibili (coerente con la guardia di deploy.ps1)."""
        drives = []
        try:
            ps_cmd = ("Get-Volume | Where-Object {$_.DriveType -eq 'Removable' -and $_.DriveLetter} | "
                      "ForEach-Object { [pscustomobject]@{ d=$_.DriveLetter; l=$_.FileSystemLabel; "
                      "s=$_.Size; f=$_.SizeRemaining } } | ConvertTo-Json -Compress")
            out = subprocess.check_output(PS + ["-Command", ps_cmd], text=True).strip()
            if out:
                data = json.loads(out)
                if isinstance(data, dict):
                    data = [data]
                for v in data:
                    label = v.get("l") or "(senza nome)"
                    gb = (v.get("s") or 0) / 1024**3
                    free = (v.get("f") or 0) / 1024**3
                    drives.append((f"{v['d']}:\\", f"{v['d']}:\\  {label}  ({free:.1f}/{gb:.1f} GB liberi)"))
        except Exception as e:
            self.log_line(f"Scan unita' fallito: {e}", "err")
        self._drive_map = {label: dev for dev, label in drives}
        labels = list(self._drive_map.keys())
        self.drive_cb["values"] = labels
        saved = self.cfg.get("drive")
        pick = next((l for l in labels if self._drive_map[l] == saved), labels[0] if labels else "")
        self.drive_var.set(pick)
        if not labels:
            self.log_line("Nessuna unita' rimovibile (inserisci la microSD).", "info")

    def selected_drive(self):
        return self._drive_map.get(self.drive_var.get(), "")

    def update_fw_info(self):
        if os.path.exists(FW_BIN):
            size = os.path.getsize(FW_BIN)
            pct = size / APP_PART_SIZE * 100
            import time
            mtime = time.strftime("%d/%m %H:%M", time.localtime(os.path.getmtime(FW_BIN)))
            warn = "  ⚠ OLTRE LA PARTIZIONE!" if size > APP_PART_SIZE else ""
            self.fw_var.set(f"nucleoos.bin: {human(size)} — {pct:.0f}% di 1.5 MB (build {mtime}){warn}")
        else:
            self.fw_var.set("nucleoos.bin: non ancora compilato")

    # ========================================================================
    #  Azioni — USB
    # ========================================================================
    def detect_chip(self):
        port = self.selected_port()
        if not self._need(port, "Seleziona una porta COM."):
            return
        self.chip_var.set("Chip: rilevamento in corso…")
        self._run(find_esptool() + ["--port", port, "chip-id"],
                  title=f"Identifico board su {port}", on_done=self._after_detect, on_done_always=True)

    def _after_detect(self, code, output):
        chip = (re.search(r"Chip type:\s*(.+)", output)
                or re.search(r"Chip is (.+)", output)
                or re.search(r"Connected to (ESP32\S*)", output))
        mac = re.search(r"MAC:\s*([0-9a-fA-F:]{17})", output)
        if chip:
            name = chip.group(1).strip()
            txt = f"Chip: {name}"
            if mac:
                txt += f"   MAC: {mac.group(1)}"
            self.chip_var.set(txt)
            if "esp32-s3" in name.lower():
                self.log_line("Board ESP32-S3 identificata ✓ (compatibile Cardputer)", "ok")
            else:
                self.log_line("⚠ Questa NON e' una ESP32-S3: il Cardputer richiede ESP32-S3!", "err")
        else:
            self.chip_var.set("Chip: non identificato — porta giusta? device occupato dal monitor?")
            self.log_line("Impossibile leggere il chip.", "err")

    def do_build(self):
        port = self.selected_port()
        if not self._need(port, "Seleziona una porta COM."):
            return
        self._run(PS + ["-File", FLASH_PS1, "-Port", port, "-BuildOnly"],
                  title=f"Build firmware ({port})", cwd=REPO, on_done=self._after_build)

    def do_build_flash(self):
        port = self.selected_port()
        if not self._need(port, "Seleziona una porta COM."):
            return
        flash = lambda *_: self._run(PS + ["-File", FLASH_PS1, "-Port", port],
                                     title=f"Build & Flash ({port})", cwd=REPO, on_done=self._after_build)
        if self.erase_var.get():
            self._run(find_esptool() + ["--port", port, "erase-flash"],
                      title=f"Erase flash ({port})", on_done=flash)  # build solo se erase ok
        else:
            flash()

    def _after_build(self, code, output):
        self.update_fw_info()

    def do_erase(self):
        port = self.selected_port()
        if not self._need(port, "Seleziona una porta COM."):
            return
        if messagebox.askyesno("Erase flash", f"Cancellare TUTTA la flash su {port}?\n"
                               "Sparisce anche la config interna (NVS: Wi-Fi salvato, setup)."):
            self._run(find_esptool() + ["--port", port, "erase-flash"], title=f"Erase flash ({port})")

    def open_monitor(self):
        port = self.selected_port()
        if not self._need(port, "Seleziona una porta COM."):
            return
        ps = f". '{IDF_EXPORT}'; Set-Location '{FIRMWARE}'; idf.py -p {port} monitor"
        try:
            subprocess.Popen(PS + ["-NoExit", "-Command", ps],
                             creationflags=getattr(subprocess, "CREATE_NEW_CONSOLE", 0))
            self.log_line(f"Monitor seriale aperto su {port} (finestra separata, Ctrl+] per uscire).", "info")
        except Exception as e:
            self.log_line(f"Monitor non avviato: {e}", "err")

    # ========================================================================
    #  Azioni — SD
    # ========================================================================
    def do_stage(self):
        args = PS + ["-File", DEPLOY_PS1]
        if self.dryrun_var.get():
            args.append("-DryRun")
        self._run(args, title="Stage repo → deploy/sd", cwd=REPO)

    def do_deploy(self):
        drive = self.selected_drive()
        if not self._need(drive, "Seleziona un'unita' SD removibile."):
            return
        dry = self.dryrun_var.get()
        if not dry and not messagebox.askyesno(
                "Deploy su SD", f"Scrivere il contenuto OS su {drive} ?\n"
                "Verranno rimossi i file estranei (mirror). L'unita' deve essere la microSD del Cardputer."):
            return
        args = PS + ["-File", DEPLOY_PS1, "-To", drive]
        if dry:
            args.append("-DryRun")
        self._run(args, title=f"Deploy → {drive}" + (" (dry-run)" if dry else ""), cwd=REPO)

    # ========================================================================
    #  Azioni — OTA
    # ========================================================================
    def do_ota(self):
        host = self.host_var.get().strip()
        if not self._need(host, "Inserisci host o IP del device."):
            return
        if not os.path.exists(FW_BIN):
            messagebox.showwarning("OTA", "Nessun firmware compilato. Fai prima un Build.")
            return
        args = PS + ["-File", OTA_PS1, "-DeviceHost", host]
        pin = self.pin_var.get().strip()
        if pin:
            args += ["-Pin", pin]
        self._run(args, title=f"OTA → {host}", cwd=REPO)

    def _on_tab_changed(self, _=None):
        """Quando apri la scheda OTA: aggiorna l'info firmware e auto-verifica online."""
        try:
            if self.nb.select() != str(self.ota_tab):
                return
        except Exception:
            return
        self.update_fw_info()
        host = self.host_var.get().strip()
        if host and not getattr(self, "_net_pending", False):
            self.check_online(auto=True)

    def check_online(self, auto=False):
        """Ping non bloccante a http://host/api/status per vedere se il device risponde."""
        host = self.host_var.get().strip()
        if not host:
            if not auto:
                self._need(host, "Inserisci host o IP del device.")
            return
        if getattr(self, "_net_pending", False):
            return
        self._net_pending = True
        self._net_auto = auto
        self.online_var.set("● verifico…")
        self.lbl_online.config(foreground="#999")
        url = f"http://{host}/api/status"
        if not auto:
            self.log_line(f"\n$ GET {url}", "cmd")

        def worker():
            import urllib.request
            try:
                with urllib.request.urlopen(url, timeout=5) as r:
                    body = r.read(4096).decode("utf-8", "replace")
                self.q.put(("net", (True, body)))
            except Exception as e:
                self.q.put(("net", (False, str(e))))

        threading.Thread(target=worker, daemon=True).start()

    def _after_online(self, ok, body):
        self._net_pending = False
        auto = getattr(self, "_net_auto", False)
        if ok:
            net = ""
            try:
                d = json.loads(body)
                n = d.get("network") or {}
                bits = [b for b in (n.get("mode"), n.get("ssid"), n.get("ip")) if b]
                if bits:
                    net = "  (" + " · ".join(str(b) for b in bits) + ")"
            except Exception:
                pass
            self.online_var.set("● online" + net)
            self.lbl_online.config(foreground="#0a0")
            if not auto:
                self.log_line("Device raggiungibile ✓ " + body.strip()[:300], "ok")
        else:
            self.online_var.set("● offline")
            self.lbl_online.config(foreground="#c00")
            if not auto:
                self.log_line(f"Device non raggiungibile: {body}", "err")

    # ========================================================================
    #  Motore esecuzione con streaming
    # ========================================================================
    def _need(self, val, msg):
        if not val:
            messagebox.showwarning("Manca un dato", msg)
            return False
        return True

    def _run(self, cmd, title="", cwd=None, on_done=None, on_done_always=False):
        if self.busy:
            messagebox.showinfo("Occupato", "Un'operazione e' gia' in corso.")
            return
        self.busy = True
        self._set_buttons(False)
        self.pb.start(12)
        self.status_var.set(title or "In esecuzione…")
        self.log_line(f"\n$ {self._pretty(cmd)}", "cmd")
        self._output_buf = []
        self._on_done = on_done
        self._on_done_always = on_done_always

        def worker():
            try:
                self.proc = subprocess.Popen(
                    cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, bufsize=1, encoding="utf-8", errors="replace",
                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
                for line in self.proc.stdout:
                    self._output_buf.append(line)
                    self.q.put(("line", line.rstrip("\n")))
                self.proc.wait()
                self.q.put(("done", self.proc.returncode))
            except FileNotFoundError as e:
                self.q.put(("line", f"ERRORE: comando non trovato: {e}"))
                self.q.put(("done", -1))
            except Exception as e:
                self.q.put(("line", f"ERRORE: {e}"))
                self.q.put(("done", -1))

        threading.Thread(target=worker, daemon=True).start()

    @staticmethod
    def _pretty(cmd):
        # accorcia il rumore di powershell -NoProfile -ExecutionPolicy Bypass -File <path>
        skip = {"-NoProfile", "-ExecutionPolicy", "Bypass", "-File"}
        parts = []
        for c in cmd:
            if c == "powershell":
                parts.append("powershell")
            elif c in skip:
                continue
            elif c.endswith(".ps1"):
                parts.append(os.path.basename(c))
            else:
                parts.append(c)
        return " ".join(parts)

    def stop_proc(self):
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.terminate()
                self.log_line("Interrotto dall'utente.", "err")
            except Exception:
                pass

    def _drain_queue(self):
        try:
            while True:
                kind, payload = self.q.get_nowait()
                if kind == "line":
                    self.log_line(payload, self._tag_for(payload))
                elif kind == "done":
                    self._finish(payload)
                elif kind == "net":
                    self._after_online(*payload)
        except queue.Empty:
            pass
        self.root.after(80, self._drain_queue)

    @staticmethod
    def _tag_for(line):
        low = line.lower()
        if any(w in low for w in ("error", "errore", "failed", "fatal", "abort", "rejected")):
            return "err"
        if any(w in low for w in ("verified", "success", "done.", "ok -", "paired", " ok (")):
            return "ok"
        return None

    def _finish(self, code):
        self.busy = False
        self.proc = None
        self.pb.stop()
        self._set_buttons(True)
        full = "".join(getattr(self, "_output_buf", []))
        if code == 0:
            self.status_var.set("Completato ✓")
            self.log_line(f"--- OK (exit {code}) ---", "ok")
        else:
            self.status_var.set(f"Fallito (exit {code})")
            self.log_line(f"--- FALLITO (exit {code}) ---", "err")
        cb, self._on_done = getattr(self, "_on_done", None), None
        if cb and (code == 0 or getattr(self, "_on_done_always", False)):
            cb(code, full)

    def _set_buttons(self, enabled):
        for b in self.action_btns:
            b.config(state="normal" if enabled else "disabled")
        self.btn_stop.config(state="disabled" if enabled else "normal")

    def log_line(self, text, tag=None):
        self.log.insert("end", text + "\n", tag or ())
        self.log.see("end")

    # ========================================================================
    def _on_close(self):
        self.cfg["port"] = self.selected_port()
        self.cfg["drive"] = self.selected_drive()
        self.cfg["host"] = self.host_var.get().strip()
        save_settings(self.cfg)
        if self.proc and self.proc.poll() is None:
            if not messagebox.askyesno("Uscire?", "Un'operazione e' in corso. Uscire comunque?"):
                return
            try:
                self.proc.terminate()
            except Exception:
                pass
        self.root.destroy()


def main():
    if not os.path.exists(FLASH_PS1):
        print(f"ATTENZIONE: flash.ps1 non trovato in {FLASH_PS1}", file=sys.stderr)
    root = tk.Tk()
    FlasherApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
