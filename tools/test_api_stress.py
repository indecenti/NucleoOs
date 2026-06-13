"""
FS stress test — "does the filesystem hold under an extraction burst?"

Writes a burst of files to a FLASHED NucleoOS device over /api/fs and proves the storage path
survives the kind of `fs.changed` storm an archive extraction produces on the PSRAM-less ESP32-S3:

  1. WRITES LAND  — every file in the burst is written (no silent drops under concurrency).
  2. INTEGRITY    — a subsequent `list` sees exactly the files that were written.
  3. GRACEFUL     — a request the single-task httpd can't serve right now returns 503/429
                    (the arbiter throttling on purpose) and is retried — NEVER counted as a loss.

Complements tools/test_arbiter_load.py: that one hammers the heavy TLS/proxy path (heap floor,
liveness); this one exercises the SD write path + the fs.changed broadcast bus.

This needs a real device. /api/fs is gated, so it pairs first (POST /api/pair with the PIN, then
sends the nucleo_session cookie). It SKIPs cleanly (exit 0) when the device is unreachable, so it
never falses-red in an offline CI run.

  python tools/test_api_stress.py --url http://192.168.0.166 [--pin 689614] [--workers 2] [--files 50]

Exit 0 = pass/skip, 1 = fail.
"""

import sys
import time
import argparse
import urllib.request
import urllib.parse
import urllib.error
import json
import logging
from concurrent.futures import ThreadPoolExecutor, as_completed


logging.basicConfig(level=logging.INFO, format="[%(levelname)s] %(message)s")
logger = logging.getLogger("fs-stress")

# Session cookie obtained from pairing, replayed on every /api/fs call.
SESSION_COOKIE: str = ""


def auth_status(base_url: str):
    """GET /api/auth/status -> dict, or None if the device is unreachable.

    Doubles as the reachability preflight: a None means "no device on this run" -> SKIP.
    """
    try:
        with urllib.request.urlopen(base_url + "/api/auth/status", timeout=8) as r:
            if r.status != 200:
                return None
            return json.loads(r.read().decode("utf-8", "replace"))
    except Exception:
        return None


def pair(base_url: str, pin: str) -> None:
    """POST /api/pair with the PIN and stash the nucleo_session cookie.

    The firmware locks out for 30s after 5 wrong PINs, so the PIN must be right.
    """
    global SESSION_COOKIE
    payload = json.dumps({"pin": pin}).encode("utf-8")
    req = urllib.request.Request(base_url + "/api/pair", data=payload, method="POST",
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as response:
            set_cookie = response.headers.get("Set-Cookie", "")
            if "nucleo_session=" in set_cookie:
                SESSION_COOKIE = set_cookie.split(";", 1)[0]
                logger.info("Pairing riuscito: cookie di sessione ottenuto.")
            else:
                logger.warning("Pairing OK ma nessun cookie ricevuto.")
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")
        raise RuntimeError(f"Pairing fallito ({e.code}): {body} — PIN errato? (lockout 30s dopo 5 tentativi)")
    except Exception as e:
        raise RuntimeError(f"Pairing fallito: {e}")


def api_request(base_url: str, op: str, path: str, method: str = "GET", data: bytes = b"",
                retries: int = 3) -> dict:
    """Call /api/fs/<op>. Retries transient throttling (503/429): on the single-task device
    server the arbiter answers 503 under load — that is correct throttling, not a failure, so it
    is retried with backoff rather than counted as a lost write."""
    encoded_path = urllib.parse.quote(path)
    url = f"{base_url}/api/fs/{op}?path={encoded_path}"
    headers = {"Cookie": SESSION_COOKIE} if SESSION_COOKIE else {}

    last_err = None
    for attempt in range(retries):
        req = urllib.request.Request(url, data=data if data else None, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=10) as response:
                if response.status >= 300:
                    raise RuntimeError(f"HTTP Error {response.status}")
                body = response.read().decode("utf-8")
                return json.loads(body) if body else {"ok": True}
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", "replace")
            if e.code in (429, 503) and attempt < retries - 1:
                last_err = f"API Error {e.code}: {body}"
                time.sleep(0.3 * (attempt + 1))
                continue
            raise RuntimeError(f"API Error {e.code}: {body}")
        except Exception as e:
            if attempt < retries - 1:
                last_err = str(e)
                time.sleep(0.3 * (attempt + 1))
                continue
            raise RuntimeError(f"Request failed: {str(e)}")
    raise RuntimeError(f"Request failed dopo {retries} tentativi: {last_err}")


def cleanup_dir(base_url: str, test_dir: str) -> None:
    """Remove a directory and its contents. The firmware delete is NOT recursive
    (remove() then rmdir(), and rmdir only deletes EMPTY dirs), so contents must go
    first, then the now-empty directory."""
    try:
        res = api_request(base_url, "list", test_dir)
    except Exception:
        return  # dir absent -> nothing to clean
    for e in res.get("entries", []):
        name = e.get("name") if isinstance(e, dict) else e
        if not name:
            continue
        try:
            api_request(base_url, "delete", f"{test_dir}/{name}", method="POST")
        except Exception:
            pass
    try:
        api_request(base_url, "delete", test_dir, method="POST")
    except Exception:
        pass


def test_stress_fs(base_url: str, workers: int = 2, num_files: int = 50) -> bool:
    """Run the burst; return True on pass, False on a real failure."""
    test_dir = "/data/stress_test"

    # 1. Preventive cleanup (ignore if absent).
    cleanup_dir(base_url, test_dir)

    # 2. Create the test directory.
    logger.info(f"Creazione cartella di test: {test_dir}")
    api_request(base_url, "mkdir", test_dir, method="POST")

    # 3. Write burst (the stress).
    logger.info(f"Scrittura di {num_files} file con {workers} worker (genera tempesta fs.changed)...")
    start_time = time.time()

    def write_dummy_file(i: int) -> bool:
        file_path = f"{test_dir}/file_{i:03d}.txt"
        payload = f"Contenuto di test per il file {i}".encode("utf-8")
        try:
            api_request(base_url, "write", file_path, method="POST", data=payload)
            return True
        except Exception as e:
            logger.error(f"Errore scrittura {file_path}: {e}")
            return False

    success_count = 0
    with ThreadPoolExecutor(max_workers=workers) as executor:
        futures = {executor.submit(write_dummy_file, i): i for i in range(num_files)}
        for future in as_completed(futures):
            if future.result():
                success_count += 1

    elapsed = time.time() - start_time
    logger.info(f"Scritti {success_count}/{num_files} file in {elapsed:.2f}s.")
    if success_count < num_files:
        logger.error("FAIL — alcuni file non sono stati scritti (anche dopo i retry).")
        return False

    # 4. Integrity check (list).
    logger.info("Verifica integrità cartella...")
    try:
        res = api_request(base_url, "list", test_dir)
        entries = res.get("entries", [])
        if len(entries) != num_files:
            logger.error(f"FAIL — trovati {len(entries)} file invece di {num_files}.")
            return False
        logger.info("Integrità OK: tutti i file sono presenti.")
    except Exception as e:
        logger.error(f"FAIL durante la list: {e}")
        return False

    # 5. Final cleanup (delete files first, then the empty dir — firmware delete isn't recursive).
    logger.info("Eliminazione cartella di test...")
    cleanup_dir(base_url, test_dir)
    if api_request_exists(base_url, test_dir):
        logger.warning("Pulizia finale incompleta: la cartella di test esiste ancora.")
    else:
        logger.info("Pulizia completata.")

    logger.info("PASS — il sistema ha retto la raffica di scritture.")
    return True


def api_request_exists(base_url: str, path: str) -> bool:
    """True if a list of `path` succeeds (i.e. the dir still exists)."""
    try:
        api_request(base_url, "list", path)
        return True
    except Exception:
        return False


def main() -> int:
    parser = argparse.ArgumentParser(description="NucleoOS FS stress test")
    parser.add_argument("--url", default="http://192.168.0.166",
                        help="URL base del device (es. http://192.168.0.166)")
    parser.add_argument("--pin", default="689614",
                        help="PIN di pairing (default 689614). Vuoto per saltare il pairing.")
    parser.add_argument("--workers", type=int, default=2,
                        help="Scritture concorrenti (default 2; tienilo basso sul device single-task)")
    parser.add_argument("--files", type=int, default=50, help="Numero di file (default 50)")
    args = parser.parse_args()
    base_url = args.url.rstrip("/")

    # --- preflight: device reachable? (else SKIP, never a false red) -------------------------------
    status = auth_status(base_url)
    if status is None:
        print(f"[fs-stress] SKIP — {base_url}/api/auth/status irraggiungibile (nessun device in questo run)")
        return 0

    # --- pair only if the device actually gates /api/fs -------------------------------------------
    if status.get("required") and not status.get("paired"):
        if not args.pin:
            print("[fs-stress] SKIP — pairing richiesto ma nessun PIN fornito (--pin)")
            return 0
        try:
            pair(base_url, args.pin)
        except Exception as e:
            logger.error(str(e))
            return 1
    elif not status.get("required"):
        logger.info("Pairing non richiesto dal device.")

    logger.info(f"Avvio test verso: {base_url}")
    try:
        ok = test_stress_fs(base_url, workers=args.workers, num_files=args.files)
    except KeyboardInterrupt:
        logger.info("Test interrotto dall'utente.")
        return 1
    except Exception as e:
        logger.error(f"Errore fatale: {e}")
        return 1
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
