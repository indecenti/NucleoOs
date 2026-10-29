"""Encrypted backups of the NucleoOS signing keys (every *.pem in %USERPROFILE%\\.nucleo).

Losing a release key means no device accepts an update over the air any more; leaking one means anyone can
ship firmware to every device. So the keys are kept in several places, always encrypted:

  python tools/keys_backup.py create E:\\nucleo-keys H:\\nucleo-keys    one bundle, copied + verified everywhere
  python tools/keys_backup.py verify E:\\nucleo-keys\\nucleo-keys-20261029.nkb
  python tools/keys_backup.py restore FILE                          back into ~/.nucleo (never overwrites)

The bundle is AES-256-GCM under a key derived from your passphrase with scrypt (N=2^17, r=8, p=1): without
the passphrase it is useless, with a wrong one it fails to open (GCM tag). The passphrase is asked for at the
terminal and never stored; write it down and keep it apart from the copies. No secret ever goes in the repo.
"""
import argparse
import datetime
import getpass
import hashlib
import json
import os
import sys

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.scrypt import Scrypt

HOME = os.path.join(os.path.expanduser("~"), ".nucleo")
MAGIC = b"NUCLEO-KEYS-1\n"
SCRYPT_N, SCRYPT_R, SCRYPT_P = 1 << 17, 8, 1


def passphrase(confirm):
    p = os.environ.get("NUCLEO_BACKUP_PASS_TEST")          # tests only
    if p:
        return p
    p = getpass.getpass("Passphrase (min 16 characters): ")
    if confirm:
        if len(p) < 16:
            sys.exit("too short: use at least 16 characters (a few random words)")
        if getpass.getpass("Again: ") != p:
            sys.exit("the two passphrases differ")
    return p


def derive(p, salt):
    return Scrypt(salt=salt, length=32, n=SCRYPT_N, r=SCRYPT_R, p=SCRYPT_P).derive(p.encode("utf-8"))


def key_fingerprint(pem):
    try:
        k = serialization.load_pem_private_key(pem.encode("ascii"), password=None)
        q = k.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
        return hashlib.sha256(q).hexdigest()[:16]
    except Exception:
        return "?"


def seal(files, p):
    body = json.dumps({"created": datetime.datetime.now().isoformat(timespec="seconds"), "files": files},
                      sort_keys=True).encode("utf-8")
    salt, nonce = os.urandom(16), os.urandom(12)
    return MAGIC + salt + nonce + AESGCM(derive(p, salt)).encrypt(nonce, body, MAGIC)


def unseal(blob, p):
    if not blob.startswith(MAGIC):
        sys.exit("not a NucleoOS key bundle")
    b = blob[len(MAGIC):]
    try:
        body = AESGCM(derive(p, b[:16])).decrypt(b[16:28], b[28:], MAGIC)
    except Exception:
        sys.exit("cannot open: wrong passphrase or damaged file")
    return json.loads(body)


def cmd_create(a):
    files = {}
    for n in sorted(os.listdir(HOME)):
        if n.endswith(".pem"):
            with open(os.path.join(HOME, n), "r", newline="") as f:
                files[n] = f.read()
    if not files:
        sys.exit("no keys in %s" % HOME)
    p = passphrase(True)
    blob = seal(files, p)
    name = "nucleo-keys-%s.nkb" % datetime.date.today().strftime("%Y%m%d")
    ok = 0
    for d in a.dest:
        try:
            os.makedirs(d, exist_ok=True)
            path = os.path.join(d, name)
            with open(path, "wb") as f:
                f.write(blob)
                f.flush()
                os.fsync(f.fileno())
            with open(path, "rb") as f:                        # read back from the medium and open it
                back = unseal(f.read(), p)["files"]
            if back != files:
                raise IOError("read-back differs")
            with open(os.path.join(d, "LEGGIMI-nucleo-keys.txt"), "w", encoding="utf-8") as f:
                f.write("Backup cifrato delle chiavi di firma NucleoOS (%d chiavi).\n"
                        "Si apre solo con la passphrase. Ripristino:\n"
                        "  python tools/keys_backup.py restore %s\n" % (len(files), name))
            print("OK   %s" % path)
            ok += 1
        except Exception as e:
            print("FAIL %s: %s" % (d, e))
    print("%d chiavi, %d/%d copie verificate" % (len(files), ok, len(a.dest)))
    sys.exit(0 if ok == len(a.dest) else 1)


def cmd_verify(a):
    with open(a.file, "rb") as f:
        b = unseal(f.read(), passphrase(False))
    print("creato %s, %d chiavi:" % (b["created"], len(b["files"])))
    for n, pem in sorted(b["files"].items()):
        here = os.path.join(HOME, n)
        same = os.path.isfile(here) and open(here, "r", newline="").read() == pem
        print("  %-42s %s  %s" % (n, key_fingerprint(pem), "= ~/.nucleo" if same else "(non su questo PC)" if not os.path.isfile(here) else "DIVERSA da ~/.nucleo"))


def cmd_restore(a):
    with open(a.file, "rb") as f:
        b = unseal(f.read(), passphrase(False))
    os.makedirs(HOME, exist_ok=True)
    for n, pem in sorted(b["files"].items()):
        if os.path.basename(n) != n or not n.endswith(".pem"):
            print("skip %r" % n)
            continue
        here = os.path.join(HOME, n)
        if os.path.exists(here):
            print("%-42s %s" % (n, "gia' presente" if open(here, "r", newline="").read() == pem else "PRESENTE E DIVERSA: non toccata"))
            continue
        with open(here, "w", newline="") as f:
            f.write(pem)
        print("%-42s ripristinata" % n)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("create"); s.add_argument("dest", nargs="+"); s.set_defaults(f=cmd_create)
    s = sub.add_parser("verify"); s.add_argument("file"); s.set_defaults(f=cmd_verify)
    s = sub.add_parser("restore"); s.add_argument("file"); s.set_defaults(f=cmd_restore)
    a = ap.parse_args()
    a.f(a)


if __name__ == "__main__":
    main()
