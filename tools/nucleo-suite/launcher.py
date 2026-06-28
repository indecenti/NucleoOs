#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NucleoOS Toolkit — launcher a carosello dei tool PC-side per il Cardputer.
NucleoOS Toolkit — carousel launcher for the PC-side Cardputer tools.

Una finestra (GUI Tkinter, bilingue IT/EN) raccoglie i tool utili e ne avvia uno
per volta in un processo a parte. / One bilingual window gathers the useful tools
and launches one at a time in its own process.

Due modalità, stessa esperienza / Two modes, same experience:
  • Sorgente / Source:  python tools/nucleo-suite/launcher.py
  • .exe:               NucleoSuite.spec -> dist/NucleoSuite/NucleoSuite.exe

Diagnostica senza GUI / headless check:  python launcher.py --selftest
"""
from __future__ import annotations

import os
import sys
import runpy
import subprocess
import threading
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tools_registry import TOOLS, TOOLS_BY_ID, CATS  # noqa: E402

APP_NAME = "NucleoOS Toolkit"
APP_VER = "1.1.0"
FROZEN = getattr(sys, "frozen", False)


# --------------------------------------------------------------------------- #
#  Risoluzione percorsi (sorgente vs .exe) / Path resolution
# --------------------------------------------------------------------------- #
def repo_root() -> Path | None:
    if FROZEN:
        start = Path(sys.executable).resolve().parent
    else:
        start = Path(__file__).resolve().parent
    for p in [start, *start.parents]:
        if (p / "firmware").is_dir() and (p / "tools").is_dir():
            return p
    return None


def script_path(tool: dict) -> Path | None:
    if FROZEN:
        p = Path(getattr(sys, "_MEIPASS", ".")) / tool["script"]
        return p if p.exists() else None
    root = repo_root()
    if not root:
        return None
    p = root / tool["script"]
    return p if p.exists() else None


def tool_available(tool: dict) -> tuple[bool, dict]:
    """(disponibile, motivo-bilingue) nella modalità corrente."""
    if FROZEN and not tool["frozen_ok"]:
        return False, {"it": "Disponibile solo da sorgente (richiede repo + toolchain).",
                       "en": "Available only from source (needs repo + toolchain)."}
    if not FROZEN and tool.get("needs_repo") and repo_root() is None:
        return False, {"it": "Repo NucleoOS non trovato da questa cartella.",
                       "en": "NucleoOS repo not found from this folder."}
    if script_path(tool) is None:
        return False, {"it": "Script non trovato.", "en": "Script not found."}
    return True, {"it": "", "en": ""}


# --------------------------------------------------------------------------- #
#  --run <id> : esegue lo script impacchettato come __main__ (modalità .exe)
# --------------------------------------------------------------------------- #
def _dispatch_run(tool_id: str, extra: list[str]) -> int:
    tool = TOOLS_BY_ID.get(tool_id)
    if not tool:
        print(f"[toolkit] tool sconosciuto / unknown tool: {tool_id}", file=sys.stderr)
        return 2
    path = script_path(tool)
    if path is None:
        print(f"[toolkit] script non trovato / not found: {tool_id}", file=sys.stderr)
        return 2
    root = repo_root()
    if root:
        os.chdir(root)
    sys.argv = [str(path), *extra]
    try:
        runpy.run_path(str(path), run_name="__main__")
        return 0
    except SystemExit as e:
        return int(e.code or 0)
    except BaseException:  # noqa: BLE001 — app windowed: l'errore sarebbe invisibile
        import traceback
        tb = traceback.format_exc()
        try:
            logf = Path(sys.executable).resolve().parent / "toolkit-error.log"
            with open(logf, "a", encoding="utf-8") as fh:
                fh.write(f"\n=== {tool_id} {extra} ===\n{tb}\n")
        except Exception:
            pass
        print(tb, file=sys.stderr)
        return 1


def _selftest() -> int:
    print(f"{APP_NAME} {APP_VER} — selftest ({'frozen' if FROZEN else 'source'})")
    print(f"repo root: {repo_root()}")
    bad = 0
    for t in TOOLS:
        ok, why = tool_available(t)
        p = script_path(t)
        detail = p or (why.get("en") if isinstance(why, dict) else why)
        if p is None and not (FROZEN and not t["frozen_ok"]):
            bad += 1
        mark = "OK " if p is not None else "!! "
        print(f"  {mark}{t['id']:<16} kind={t['kind']:<3} frozen_ok={t['frozen_ok']!s:<5} "
              f"avail={ok!s:<5} -> {detail}")
    print("RESULT:", "FAIL" if bad else "PASS", f"({bad} script mancanti)")
    return 1 if bad else 0


# =========================================================================== #
#  GUI
# =========================================================================== #
def run_gui() -> int:
    import tkinter as tk
    from tkinter import filedialog, messagebox

    # palette (vibe NucleoOS: spazio/atomo)
    BG, PANEL, CARD, EDGE = "#0b0f17", "#131a26", "#172131", "#243349"
    TXT, MUT, DIM = "#e6edf6", "#8aa0b8", "#5b6b80"
    ACC, OKC, WARN = "#4ea1ff", "#7ee787", "#ffb454"
    FONT = "Segoe UI"

    # ---- testi di interfaccia bilingui / bilingual UI strings ----
    UI = {
        "subtitle": {"it": "Strumenti PC-side per il Cardputer", "en": "PC-side tools for the Cardputer"},
        "mode_exe": {"it": "pacchetto .exe", "en": ".exe package"},
        "mode_src": {"it": "modalità sorgente", "en": "source mode"},
        "run": {"it": "▶  Avvia", "en": "▶  Run"},
        "open": {"it": "📁  Apri cartella", "en": "📁  Open folder"},
        "requires": {"it": "Richiede:", "en": "Requires:"},
        "output": {"it": "Output", "en": "Output"},
        "clear": {"it": "pulisci", "en": "clear"},
        "ready": {"it": "Pronto. Scegli uno strumento e premi Avvia (Invio).",
                  "en": "Ready. Pick a tool and press Run (Enter)."},
        "no_repo": {"it": "Attenzione: repo NucleoOS non trovato — alcuni tool sono disattivati.",
                    "en": "Warning: NucleoOS repo not found — some tools are disabled."},
        "launched": {"it": "Avviato", "en": "Launched"},
        "cannot": {"it": "Impossibile avviare", "en": "Cannot launch"},
        "no_path": {"it": "Percorso non disponibile in questa modalità.",
                    "en": "Path not available in this mode."},
        "no_open": {"it": "Impossibile aprire la cartella", "en": "Cannot open folder"},
        "empty_cat": {"it": "Nessun tool in questa categoria.", "en": "No tools in this category."},
        "category": {"it": "categoria", "en": "category"},
        "run_dlg": {"it": "Esegui", "en": "Run"},
        "cancel": {"it": "Annulla", "en": "Cancel"},
        "browse": {"it": "Sfoglia", "en": "Browse"},
        "required": {"it": "Campo obbligatorio:", "en": "Required field:"},
        "badge_src_only": {"it": "SOLO SORGENTE", "en": "SOURCE ONLY"},
        "badge_repo": {"it": "RICHIEDE REPO", "en": "NEEDS REPO"},
        # tooltips
        "tip_prev": {"it": "Strumento precedente (←)", "en": "Previous tool (←)"},
        "tip_next": {"it": "Strumento successivo (→)", "en": "Next tool (→)"},
        "tip_lang": {"it": "Cambia lingua dell'interfaccia (Italiano / Inglese)",
                     "en": "Switch interface language (Italian / English)"},
        "tip_filter": {"it": "Filtra gli strumenti per categoria", "en": "Filter tools by category"},
        "tip_run_gui": {"it": "Apre lo strumento in una nuova finestra.",
                        "en": "Opens the tool in a new window."},
        "tip_run_cli": {"it": "Chiede i parametri e mostra l'output qui sotto nel pannello.",
                        "en": "Asks for parameters and shows the output in the panel below."},
        "tip_run_off": {"it": "Non disponibile in questa modalità (vedi la nota accanto).",
                        "en": "Not available in this mode (see the note next to it)."},
        "tip_open": {"it": "Apre nel file manager la cartella che contiene lo script del tool.",
                     "en": "Opens the folder containing the tool's script in your file manager."},
        "tip_dots": {"it": "Vai a questo strumento", "en": "Jump to this tool"},
        "tip_clear": {"it": "Svuota il pannello di output", "en": "Clear the output panel"},
        "tip_card": {"it": "Premi Avvia (o Invio) per eseguire questo strumento.",
                     "en": "Press Run (or Enter) to execute this tool."},
    }

    state = {"index": 0, "filter": "all", "lang": "it"}
    filtered: list[dict] = list(TOOLS)
    tips: list = []   # _Tip vivi, per aggiornarli al cambio lingua

    def L(val):
        if isinstance(val, dict):
            return val.get(state["lang"], val.get("it", ""))
        return val

    root = tk.Tk()
    root.title(f"{APP_NAME}  v{APP_VER}")
    root.configure(bg=BG)
    root.minsize(900, 640)
    W, H = 980, 730
    sw, sh = root.winfo_screenwidth(), root.winfo_screenheight()
    root.geometry(f"{W}x{H}+{(sw - W) // 2}+{max(0, (sh - H) // 2 - 20)}")

    # ---- tooltip (Tkinter non ne ha uno nativo) / tooltip helper ----
    class _Tip:
        def __init__(self, widget, getter):
            self.w = widget; self.getter = getter; self.tip = None; self.id = None
            widget.bind("<Enter>", lambda _: self._schedule(), add="+")
            widget.bind("<Leave>", lambda _: self._cancel(), add="+")
            widget.bind("<ButtonPress>", lambda _: self._cancel(), add="+")
            tips.append(self)
        def _schedule(self):
            self._cancel(); self.id = self.w.after(450, self._show)
        def _cancel(self):
            if self.id:
                self.w.after_cancel(self.id); self.id = None
            if self.tip:
                self.tip.destroy(); self.tip = None
        def _show(self):
            if self.tip:
                return
            try:
                x = self.w.winfo_rootx() + 16
                y = self.w.winfo_rooty() + self.w.winfo_height() + 6
            except Exception:
                return
            txt = self.getter()
            if not txt:
                return
            self.tip = tw = tk.Toplevel(self.w)
            tw.wm_overrideredirect(True)
            tw.wm_geometry(f"+{x}+{y}")
            tk.Label(tw, text=txt, justify="left", bg="#fffbe6", fg="#222",
                     relief="solid", bd=1, font=(FONT, 9), wraplength=360,
                     padx=9, pady=6).pack()

    def tip(widget, getter):
        _Tip(widget, getter)
        return widget

    # ---- icone vettoriali (no asset binari) ----
    def draw_glyph(cv, kind, color, s=92):
        cv.delete("all")
        import math
        m = s * 0.16
        cv.create_oval(2, 2, s - 2, s - 2, fill="#0e1622", outline=EDGE)
        c = s / 2
        if kind == "film":
            cv.create_rectangle(m, m * 1.4, s - m, s - m * 1.4, outline=color, width=3)
            for i in range(4):
                y = m * 1.4 + (s - 2 * m * 1.4) * (i + 0.5) / 4
                cv.create_rectangle(m * 0.45, y - 3, m * 0.95, y + 3, fill=color, outline="")
                cv.create_rectangle(s - m * 0.95, y - 3, s - m * 0.45, y + 3, fill=color, outline="")
            cv.create_polygon(c - 9, c - 12, c - 9, c + 12, c + 13, c, fill=color, outline="")
        elif kind == "reindex":
            cv.create_arc(m, m, s - m, s - m, start=40, extent=250, style="arc", outline=color, width=4)
            cv.create_polygon(s - m * 1.5, m * 1.2, s - m * 0.4, m * 1.6, s - m * 1.3, m * 2.4,
                              fill=color, outline="")
        elif kind == "sd":
            x0, y0, x1, y1 = m, m * 0.8, s - m, s - m * 0.8
            cv.create_polygon(x0 + (x1 - x0) * 0.32, y0, x1, y0, x1, y1, x0, y1, x0,
                              y0 + (y1 - y0) * 0.28, x0 + (x1 - x0) * 0.32, y0,
                              fill="", outline=color, width=3)
            for i in range(5):
                xx = x0 + (x1 - x0) * (0.38 + i * 0.12)
                cv.create_line(xx, y0 + 6, xx, y0 + 18, fill=color, width=3)
        elif kind == "bolt":
            cv.create_polygon(c + 8, m, m * 1.4, c + 6, c - 2, c + 6, c - 8, s - m, s - m * 1.3,
                              c - 6, c + 4, c - 6, fill=color, outline="")
        elif kind == "terminal":
            cv.create_rectangle(m, m, s - m, s - m, outline=color, width=3)
            cv.create_line(m, m * 1.9, s - m, m * 1.9, fill=color, width=2)
            for i, cc in enumerate(("#ff6b6b", "#ffd93d", "#6bffb0")):
                cv.create_oval(m + 6 + i * 12, m * 1.25, m + 14 + i * 12, m * 1.55, fill=cc, outline="")
            cv.create_text(c - 6, c + 8, text=">_", fill=color, font=(FONT, int(s * 0.2), "bold"))
        elif kind == "wave":
            pts = []
            for i in range(0, 41):
                x = m + (s - 2 * m) * i / 40
                y = c + math.sin(i / 40 * math.pi * 3) * (s * 0.18)
                pts += [x, y]
            cv.create_line(*pts, fill=color, width=3, smooth=True)
        else:
            cv.create_text(c, c, text="?", fill=color, font=(FONT, 30, "bold"))

    # ---- header ----
    header = tk.Frame(root, bg=BG)
    header.pack(fill="x", padx=22, pady=(18, 6))
    atom = tk.Canvas(header, width=54, height=54, bg=BG, highlightthickness=0)
    atom.pack(side="left")
    atom.create_oval(23, 23, 31, 31, fill=ACC, outline="")
    for ang in (0, 60, 120):
        atom.create_arc(8, 18, 46, 36, start=ang, extent=180, style="arc", outline="#33507a", width=2)
    atom.create_oval(7, 24, 13, 30, fill="#9ad0ff", outline="")
    atom.create_oval(41, 24, 47, 30, fill="#9ad0ff", outline="")
    atom.create_oval(24, 7, 30, 13, fill="#c7b3ff", outline="")
    tit = tk.Frame(header, bg=BG)
    tit.pack(side="left", padx=12)
    tk.Label(tit, text=APP_NAME, bg=BG, fg=TXT, font=(FONT, 19, "bold")).pack(anchor="w")
    subtitle_lbl = tk.Label(tit, bg=BG, fg=MUT, font=(FONT, 10))
    subtitle_lbl.pack(anchor="w")

    # lingua (IT/EN) a destra
    langbar = tk.Frame(header, bg=BG)
    langbar.pack(side="right")
    lang_btns = {}

    def set_lang(lang):
        state["lang"] = lang
        for code, b in lang_btns.items():
            b.config(fg=TXT if code == lang else DIM, bg=PANEL if code == lang else BG)
        relabel()

    for code in ("it", "en"):
        b = tk.Button(langbar, text=code.upper(), bd=0, relief="flat", cursor="hand2",
                      bg=BG, fg=DIM, activebackground=PANEL, activeforeground=TXT,
                      font=(FONT, 10, "bold"), padx=9, pady=4,
                      command=lambda c=code: set_lang(c))
        b.pack(side="left", padx=2)
        lang_btns[code] = b
        tip(b, lambda: L(UI["tip_lang"]))

    # ---- filtro categorie ----
    filt = tk.Frame(root, bg=BG)
    filt.pack(fill="x", padx=22)
    cat_btns = {}

    def set_filter(cat):
        state["filter"] = cat
        nonlocal filtered
        filtered = [t for t in TOOLS if cat == "all" or t["cat"] == cat]
        state["index"] = 0
        for c, (b, _name) in cat_btns.items():
            b.config(fg=TXT if c == cat else DIM, bg=PANEL if c == cat else BG)
        render()

    for key, name in CATS:
        b = tk.Button(filt, bd=0, relief="flat", cursor="hand2",
                      bg=BG, fg=DIM, activebackground=PANEL, activeforeground=TXT,
                      font=(FONT, 10), padx=10, pady=4,
                      command=lambda k=key: set_filter(k))
        b.pack(side="left", padx=2)
        cat_btns[key] = (b, name)
        tip(b, lambda: L(UI["tip_filter"]))

    # ---- area carosello ----
    stage = tk.Frame(root, bg=BG)
    stage.pack(fill="both", expand=True, padx=22, pady=6)

    def nav(delta):
        if filtered:
            state["index"] = (state["index"] + delta) % len(filtered)
            render()

    arrow_l = tk.Button(stage, text="‹", bd=0, bg=BG, fg=MUT, activebackground=BG,
                        activeforeground=ACC, font=(FONT, 34, "bold"), cursor="hand2",
                        width=2, command=lambda: nav(-1))
    arrow_l.pack(side="left", fill="y")
    tip(arrow_l, lambda: L(UI["tip_prev"]))
    arrow_r = tk.Button(stage, text="›", bd=0, bg=BG, fg=MUT, activebackground=BG,
                        activeforeground=ACC, font=(FONT, 34, "bold"), cursor="hand2",
                        width=2, command=lambda: nav(1))
    arrow_r.pack(side="right", fill="y")
    tip(arrow_r, lambda: L(UI["tip_next"]))

    card = tk.Frame(stage, bg=CARD, highlightbackground=EDGE, highlightthickness=1)
    card.pack(side="left", fill="both", expand=True, padx=8)

    dots = tk.Frame(root, bg=BG)
    dots.pack(fill="x", pady=(2, 6))
    dots_inner = tk.Frame(dots, bg=BG)
    dots_inner.pack()

    # ---- pannello output ----
    outwrap = tk.Frame(root, bg=BG)
    outwrap.pack(fill="both", padx=22, pady=(0, 6))
    obar = tk.Frame(outwrap, bg=BG)
    obar.pack(fill="x")
    out_lbl = tk.Label(obar, bg=BG, fg=MUT, font=(FONT, 9, "bold"))
    out_lbl.pack(side="left")
    out = tk.Text(outwrap, height=7, bg="#0a0e15", fg="#b8c7da", bd=0, insertbackground=TXT,
                  font=("Consolas", 9), wrap="word", padx=10, pady=6, state="disabled")
    out.pack(fill="both", expand=True, pady=(3, 0))
    out.tag_config("ok", foreground=OKC)
    out.tag_config("err", foreground="#ff7b72")
    out.tag_config("dim", foreground=DIM)

    def log(msg, tag=""):
        out.config(state="normal")
        out.insert("end", msg + "\n", tag)
        out.see("end")
        out.config(state="disabled")

    def clear_out():
        out.config(state="normal"); out.delete("1.0", "end"); out.config(state="disabled")

    clr_btn = tk.Button(obar, bd=0, bg=BG, fg=DIM, activebackground=BG, activeforeground=TXT,
                        font=(FONT, 9), cursor="hand2", command=clear_out)
    clr_btn.pack(side="right")
    tip(clr_btn, lambda: L(UI["tip_clear"]))

    status = tk.Label(root, bg=PANEL, fg=DIM, font=(FONT, 9), anchor="w", padx=12, pady=4)
    status.pack(fill="x", side="bottom")

    # --------------------------------------------------------------------- #
    #  Esecuzione dei tool
    # --------------------------------------------------------------------- #
    def gui_python():
        if FROZEN:
            return sys.executable
        pw = Path(sys.executable).with_name("pythonw.exe")
        return str(pw) if pw.exists() else sys.executable

    def build_cmd(tool, args, gui):
        if FROZEN:
            return [sys.executable, "--run", tool["id"], *args], None
        exe = gui_python() if gui else sys.executable
        rd = repo_root()
        return [exe, str(script_path(tool)), *args], (str(rd) if rd else None)

    def stream(cmd, cwd, title):
        log(f"$ {title} …", "dim")

        def worker():
            try:
                flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
                p = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                                     errors="replace", bufsize=1, creationflags=flags)
                for line in p.stdout:  # type: ignore[union-attr]
                    root.after(0, log, line.rstrip("\n"))
                p.wait()
                fin = "finito" if state["lang"] == "it" else "finished"
                root.after(0, log, f"— {fin} (exit {p.returncode}) —",
                           "ok" if p.returncode == 0 else "err")
            except Exception as e:  # noqa: BLE001
                root.after(0, log, f"errore / error: {e}", "err")

        threading.Thread(target=worker, daemon=True).start()

    def spawn(cmd, cwd, title):
        try:
            subprocess.Popen(cmd, cwd=cwd)
            log(f"{L(UI['launched'])}: {title}", "ok")
        except Exception as e:  # noqa: BLE001
            log(f"{L(UI['cannot'])} {title}: {e}", "err")

    def collect_params(tool):
        spec = tool.get("params", [])
        if not spec:
            return []
        dlg = tk.Toplevel(root, bg=PANEL)
        dlg.title(L(tool["title"]))
        dlg.configure(padx=18, pady=16)
        dlg.transient(root); dlg.grab_set()
        tk.Label(dlg, text=L(tool["title"]), bg=PANEL, fg=TXT, font=(FONT, 13, "bold")).pack(anchor="w")
        tk.Label(dlg, text=L(tool["tagline"]), bg=PANEL, fg=MUT, font=(FONT, 9),
                 wraplength=380, justify="left").pack(anchor="w", pady=(0, 10))
        vars_ = {}
        for f in spec:
            row = tk.Frame(dlg, bg=PANEL); row.pack(fill="x", pady=4)
            if f["type"] == "bool":
                v = tk.BooleanVar(value=bool(f.get("default", False)))
                tk.Checkbutton(row, text=L(f["label"]), variable=v, bg=PANEL, fg=TXT,
                               selectcolor=CARD, activebackground=PANEL, activeforeground=TXT,
                               font=(FONT, 10)).pack(anchor="w")
                vars_[f["name"]] = v
                continue
            tk.Label(row, text=L(f["label"]), bg=PANEL, fg=MUT, font=(FONT, 9)).pack(anchor="w")
            line = tk.Frame(row, bg=PANEL); line.pack(fill="x")
            default = f.get("default", "")
            if f["type"] in ("path", "savepath") and default:
                rr = repo_root()
                if rr and not os.path.isabs(str(default)):
                    default = str(rr / default)
            v = tk.StringVar(value=str(default))
            tk.Entry(line, textvariable=v, bg="#0a0e15", fg=TXT, bd=0, insertbackground=TXT,
                     font=("Consolas", 10)).pack(side="left", fill="x", expand=True, ipady=4, padx=(0, 6))
            vars_[f["name"]] = v
            if f["type"] == "path":
                def pick(var=v, mode=f.get("mode", "any")):
                    p = (filedialog.askdirectory() if mode == "dir" else filedialog.askopenfilename())
                    if not p and mode == "any":
                        p = filedialog.askdirectory()
                    if p:
                        var.set(p)
                tk.Button(line, text=L(UI["browse"]), bd=0, bg=CARD, fg=TXT, activebackground=EDGE,
                          font=(FONT, 9), cursor="hand2", command=pick).pack(side="right")
            elif f["type"] == "savepath":
                def pick(var=v):
                    p = filedialog.asksaveasfilename()
                    if p:
                        var.set(p)
                tk.Button(line, text=L(UI["browse"]), bd=0, bg=CARD, fg=TXT, activebackground=EDGE,
                          font=(FONT, 9), cursor="hand2", command=pick).pack(side="right")

        result = {"argv": None}

        def ok():
            argv = []
            for f in spec:
                val = vars_[f["name"]]
                if f["type"] == "bool":
                    if val.get():
                        argv.append(f["flag"])
                else:
                    s = str(val.get()).strip()
                    if not s and f.get("required"):
                        messagebox.showwarning(APP_NAME, f"{L(UI['required'])} {L(f['label'])}")
                        return
                    if s:
                        argv.append(s)
            result["argv"] = argv
            dlg.destroy()

        btns = tk.Frame(dlg, bg=PANEL); btns.pack(fill="x", pady=(14, 0))
        tk.Button(btns, text=L(UI["cancel"]), bd=0, bg=CARD, fg=MUT, activebackground=EDGE,
                  font=(FONT, 10), cursor="hand2", padx=14, pady=6, command=dlg.destroy).pack(side="right", padx=4)
        tk.Button(btns, text=L(UI["run_dlg"]), bd=0, bg=ACC, fg="#04101f", activebackground="#6fb6ff",
                  font=(FONT, 10, "bold"), cursor="hand2", padx=16, pady=6, command=ok).pack(side="right")
        dlg.bind("<Return>", lambda e: ok())
        dlg.wait_window()
        return result["argv"]

    def run_tool(tool):
        ok, why = tool_available(tool)
        if not ok:
            log(f"{L(tool['title'])}: {L(why)}", "err")
            return
        args = collect_params(tool)
        if args is None:
            return
        gui = tool["kind"] == "gui"
        cmd, cwd = build_cmd(tool, args, gui)
        (spawn if gui else stream)(cmd, cwd, L(tool["title"]))

    def open_location(tool):
        p = script_path(tool)
        if not p:
            log(L(UI["no_path"]), "err")
            return
        try:
            if os.name == "nt":
                os.startfile(str(p.parent))  # noqa: S606
            else:
                subprocess.Popen(["xdg-open", str(p.parent)])
        except Exception as e:  # noqa: BLE001
            log(f"{L(UI['no_open'])}: {e}", "err")

    # --------------------------------------------------------------------- #
    #  Render
    # --------------------------------------------------------------------- #
    def render():
        for w in card.winfo_children():
            w.destroy()
        for w in dots_inner.winfo_children():
            w.destroy()
        if not filtered:
            tk.Label(card, text=L(UI["empty_cat"]), bg=CARD, fg=MUT, font=(FONT, 12)).pack(expand=True)
            return

        i = state["index"]
        t = filtered[i]
        avail, why = tool_available(t)
        acc = t["accent"]
        catname = dict((k, n) for k, n in CATS).get(t["cat"], {"it": t["cat"], "en": t["cat"]})

        body = tk.Frame(card, bg=CARD)
        body.pack(fill="both", expand=True, padx=28, pady=22)

        top = tk.Frame(body, bg=CARD); top.pack(fill="x")
        icv = tk.Canvas(top, width=92, height=92, bg=CARD, highlightthickness=0)
        icv.pack(side="left")
        draw_glyph(icv, t["glyph"], acc)
        tip(icv, lambda: L(UI["tip_card"]))
        head = tk.Frame(top, bg=CARD); head.pack(side="left", fill="x", expand=True, padx=18)
        tk.Label(head, text=L(t["title"]), bg=CARD, fg=TXT, font=(FONT, 20, "bold")).pack(anchor="w")
        tk.Label(head, text=L(t["tagline"]), bg=CARD, fg=MUT, font=(FONT, 11),
                 wraplength=520, justify="left").pack(anchor="w", pady=(2, 8))
        badges = tk.Frame(head, bg=CARD); badges.pack(anchor="w")

        def badge(parent, text, fg, bgc):
            tk.Label(parent, text=text, bg=bgc, fg=fg, font=(FONT, 8, "bold"),
                     padx=8, pady=2).pack(side="left", padx=(0, 6))

        badge(badges, L(catname).upper(), "#04101f", acc)
        if FROZEN:
            badge(badges, "EXE", "#04101f", OKC) if t["frozen_ok"] \
                else badge(badges, L(UI["badge_src_only"]), "#1a1206", WARN)
        elif t.get("needs_repo"):
            badge(badges, L(UI["badge_repo"]), "#1a1206", WARN)
        badge(badges, "GUI" if t["kind"] == "gui" else "CLI", TXT, EDGE)

        tk.Label(body, text=L(t["description"]), bg=CARD, fg="#c9d6e6", font=(FONT, 11),
                 wraplength=640, justify="left").pack(anchor="w", pady=(16, 10))
        for feat in L(t["features"]):
            row = tk.Frame(body, bg=CARD); row.pack(fill="x", pady=1)
            tk.Label(row, text="▸", bg=CARD, fg=acc, font=(FONT, 11)).pack(side="left")
            tk.Label(row, text=feat, bg=CARD, fg="#b7c5d6", font=(FONT, 10),
                     wraplength=600, justify="left").pack(side="left", padx=6)

        req = tk.Frame(body, bg=CARD); req.pack(anchor="w", pady=(14, 0))
        tk.Label(req, text=L(UI["requires"]), bg=CARD, fg=DIM, font=(FONT, 9, "bold")).pack(side="left")
        for r in L(t["requires"]):
            tk.Label(req, text=r, bg=PANEL, fg=MUT, font=(FONT, 8), padx=7, pady=2).pack(side="left", padx=(6, 0))

        actions = tk.Frame(body, bg=CARD); actions.pack(fill="x", side="bottom", pady=(20, 0))
        run_btn = tk.Button(actions, text=L(UI["run"]), bd=0,
                            bg=acc if avail else EDGE, fg="#04101f" if avail else DIM,
                            activebackground="#ffffff", font=(FONT, 12, "bold"),
                            cursor="hand2" if avail else "arrow", padx=26, pady=9,
                            state="normal" if avail else "disabled", command=lambda: run_tool(t))
        run_btn.pack(side="left")
        tip(run_btn, lambda tt=t, av=avail: L(UI["tip_run_off"]) if not av
            else (L(UI["tip_run_gui"]) if tt["kind"] == "gui" else L(UI["tip_run_cli"])))
        open_btn = tk.Button(actions, text=L(UI["open"]), bd=0, bg=PANEL, fg=TXT, activebackground=EDGE,
                             font=(FONT, 10), cursor="hand2", padx=14, pady=9, command=lambda: open_location(t))
        open_btn.pack(side="left", padx=10)
        tip(open_btn, lambda: L(UI["tip_open"]))
        if not avail:
            tk.Label(actions, text=L(why), bg=CARD, fg=WARN, font=(FONT, 9),
                     wraplength=320, justify="left").pack(side="left", padx=10)

        for j in range(len(filtered)):
            on = j == i
            d = tk.Label(dots_inner, text="●", bg=BG, fg=acc if on else "#2c3a4f",
                         font=(FONT, 12 if on else 9), cursor="hand2")
            d.pack(side="left", padx=3)
            d.bind("<Button-1>", lambda e, k=j: (state.update(index=k), render()))
            tip(d, lambda: L(UI["tip_dots"]))

        status.config(text=f"{L(t['title'])}  ·  {i + 1}/{len(filtered)}  ·  "
                           f"{L(UI['category'])} {L(catname)}")

    def relabel():
        """Riapplica tutti i testi di chrome alla lingua corrente + re-render."""
        mode = L(UI["mode_exe"]) if FROZEN else L(UI["mode_src"])
        subtitle_lbl.config(text=f"{L(UI['subtitle'])} · {mode}")
        for key, (b, name) in cat_btns.items():
            b.config(text=L(name))
        out_lbl.config(text=L(UI["output"]))
        clr_btn.config(text=L(UI["clear"]))
        render()

    # ---- bind tastiera ----
    root.bind("<Left>", lambda e: nav(-1))
    root.bind("<Right>", lambda e: nav(1))
    root.bind("<Return>", lambda e: filtered and run_tool(filtered[state["index"]]))
    root.bind("<Escape>", lambda e: root.destroy())

    set_lang("it")
    set_filter("all")
    log(L(UI["ready"]), "dim")
    if not FROZEN and repo_root() is None:
        log(L(UI["no_repo"]), "err")
    if os.environ.get("TOOLKIT_SMOKE"):
        def _cycle(n=[0]):
            seq = list(range(len(filtered)))
            if n[0] < len(seq):
                state["index"] = seq[n[0]]; render(); n[0] += 1
                root.after(90, _cycle)
            elif n[0] == len(seq):
                set_lang("en"); n[0] += 1
                root.after(200, _cycle)
            else:
                root.destroy()
        root.after(300, _cycle)
    root.mainloop()
    return 0


# --------------------------------------------------------------------------- #
def main(argv: list[str]) -> int:
    if argv and argv[0] == "--run":
        if len(argv) < 2:
            print("--run richiede un id / needs an id", file=sys.stderr)
            return 2
        return _dispatch_run(argv[1], argv[2:])
    if argv and argv[0] == "--selftest":
        return _selftest()
    return run_gui()


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
