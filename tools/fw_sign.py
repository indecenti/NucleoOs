"""Release signatures for NucleoOS (Cardputer): the firmware image and the SD content manifest.

Format and rules: docs/release-signing.md (device side: firmware/components/nucleo_fwsig).

Private keys never enter the repo or CI:
  primary  %USERPROFILE%\\.nucleo\\cardputer-ota-signing-key.pem          (this PC, signs every release)
  backup   %USERPROFILE%\\.nucleo\\cardputer-ota-signing-backup-key.pem   (keep OFFLINE: USB stick in a safe)
Their public halves live in firmware/components/nucleo_fwsig/keys/ and are compiled into the firmware
(fwsig_keys.c, generated). Every firmware trusts both, so a lost or leaked primary is replaced by a release
signed with the backup key whose firmware embeds the new primary.

Lock-out guard: an image is signed only if it embeds the primary public key, so a build that would stop
trusting our key can never be shipped signed.

  python tools/fw_sign.py keygen [--backup]        one-time: new key pair (refuses to overwrite)
  python tools/fw_sign.py keys                     regenerate fwsig_keys.c from keys/*.pub.pem
  python tools/fw_sign.py fingerprint
  python tools/fw_sign.py sign-bin --bin IN --out OUT [--backup]
  python tools/fw_sign.py sign-manifest --manifest M --out SIG [--backup]
  python tools/fw_sign.py verify --bin B | --manifest M --sig S
  python tools/fw_sign.py release vX.Y.Z [--dry-run]   sign a published GitHub release in place, re-run Pages
"""
import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
COMP = os.path.join(ROOT, "firmware", "components", "nucleo_fwsig")
PUB = {"primary": os.path.join(COMP, "keys", "primary.pub.pem"), "backup": os.path.join(COMP, "keys", "backup.pub.pem")}
KEYS_C = os.path.join(COMP, "fwsig_keys.c")
HOME = os.path.join(os.path.expanduser("~"), ".nucleo")
PRIV = {"primary": os.environ.get("NUCLEO_FW_KEY") or os.path.join(HOME, "cardputer-ota-signing-key.pem"),
        "backup": os.path.join(HOME, "cardputer-ota-signing-backup-key.pem")}
REPO = "indecenti/NucleoOs"

BLOCK = 128
MAGIC = b"nucleoos-sig-v1\n"
DOMAIN_OTA = "nucleoos-cardputer-ota-v1"
DOMAIN_SD = "nucleoos-cardputer-sd-v1"


def point(pub):
    return pub.public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)


def key_id(pub):
    return hashlib.sha256(point(pub)).hexdigest()[:16]


def load_pub(which):
    with open(PUB[which], "rb") as f:
        return serialization.load_pem_public_key(f.read())


def load_priv(which):
    p = PRIV[which]
    if not os.path.isfile(p):
        sys.exit("signing key not found: %s (python tools/fw_sign.py keygen%s)" % (p, " --backup" if which == "backup" else ""))
    with open(p, "rb") as f:
        k = serialization.load_pem_private_key(f.read(), password=None)
    if point(k.public_key()) != point(load_pub(which)):
        sys.exit("%s does not match the %s public key in the repo" % (p, which))
    return k


def message(domain, version, sha_hex, size):
    return ("%s\n%s\n%s\n%d\n" % (domain, version, sha_hex, size)).encode("ascii")


def make_block(priv, domain, version, data):
    der = priv.sign(message(domain, version, hashlib.sha256(data).hexdigest(), len(data)), ec.ECDSA(hashes.SHA256()))
    assert 8 <= len(der) <= 72
    b = MAGIC + key_id(priv.public_key()).encode("ascii") + struct.pack("<H", len(der)) + der
    return b + b"\0" * (BLOCK - len(b))


def check_block(block, domain, version, data):
    """'ok' or why not — the device's fwsig_verify, in Python."""
    if len(block) != BLOCK or block[:16] != MAGIC:
        return "unsigned"
    kid = block[16:32].decode("ascii", "replace")
    keys = {key_id(load_pub(w)): load_pub(w) for w in PUB if os.path.isfile(PUB[w])}
    if kid not in keys:
        return "unknown key %s" % kid
    dl = struct.unpack("<H", block[32:34])[0]
    try:
        keys[kid].verify(block[34:34 + dl], message(domain, version, hashlib.sha256(data).hexdigest(), len(data)),
                         ec.ECDSA(hashes.SHA256()))
    except InvalidSignature:
        return "bad signature"
    return "ok"


def image_desc(data):
    """(version, project_name) from the image's esp_app_desc (offset 0x20 of an app image)."""
    if len(data) < 0x70 or data[0] != 0xE9 or struct.unpack("<I", data[0x20:0x24])[0] != 0xABCD5432:
        sys.exit("not an ESP app image (or a merged 0x0 image: sign the app-only nucleoos.bin)")
    cstr = lambda b: b.split(b"\0")[0].decode("ascii")
    return cstr(data[0x30:0x50]), cstr(data[0x50:0x70])


def is_signed(data):
    return len(data) > BLOCK and data[-BLOCK:-BLOCK + 16] == MAGIC


def sign_bin(data, which):
    if is_signed(data):
        sys.exit("already signed")
    version, project = image_desc(data)
    if project != "nucleoos":
        sys.exit("project_name is %r, not nucleoos" % project)
    if point(load_pub("primary")) not in data:
        sys.exit("refused: this image does not embed the primary release key (it would lock devices out)")
    return data + make_block(load_priv(which), DOMAIN_OTA, version, data), version


def manifest_tag(data):
    m = re.match(rb"#nucleoos-sd 1 (\S+) ", data)
    if not m:
        sys.exit("not an SD manifest (no '#nucleoos-sd 1 <tag>' header)")
    return m.group(1).decode("ascii")


def gen_keys_c():
    rows = []
    for w in ("primary", "backup"):
        if not os.path.isfile(PUB[w]):
            continue
        q = point(load_pub(w))
        hexes = ", ".join("0x%02x" % b for b in q)
        lines = [hexes[i:i + 78] for i in range(0, len(hexes), 78)]
        rows.append('    { "%s", {  // %s\n        %s,\n    } },' % (key_id(load_pub(w)), w,
                    "\n        ".join(l.strip().rstrip(",") + ("," if j < len(lines) - 1 else "") for j, l in enumerate(lines))))
    src = ("// GENERATED by `python tools/fw_sign.py keys` from keys/*.pub.pem - do not edit.\n"
           "// The release public keys (primary + backup) this firmware trusts for firmware and SD content.\n"
           "#include \"fwsig.h\"\n\nconst fwsig_key_t FWSIG_KEYS[] = {\n%s\n};\nconst int FWSIG_NKEYS = %d;\n"
           % ("\n".join(rows), len(rows)))
    with open(KEYS_C, "w", newline="\n") as f:
        f.write(src)
    print("wrote", KEYS_C, "(%d key%s)" % (len(rows), "" if len(rows) == 1 else "s"))


def cmd_keygen(a):
    which = "backup" if a.backup else "primary"
    if os.path.exists(PRIV[which]) or os.path.exists(PUB[which]):
        sys.exit("refused: %s key already exists" % which)
    os.makedirs(os.path.dirname(PRIV[which]), exist_ok=True)
    os.makedirs(os.path.dirname(PUB[which]), exist_ok=True)
    k = ec.generate_private_key(ec.SECP256R1())
    with open(PRIV[which], "wb") as f:
        f.write(k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    with open(PUB[which], "wb") as f:
        f.write(k.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
    print("%s key %s -> %s" % (which, key_id(k.public_key()), PRIV[which]))
    gen_keys_c()


def cmd_fingerprint(a):
    for w in PUB:
        if os.path.isfile(PUB[w]):
            print("%-8s %s  private: %s" % (w, key_id(load_pub(w)), "present" if os.path.isfile(PRIV[w]) else "not on this PC"))


def cmd_sign_bin(a):
    with open(a.bin, "rb") as f:
        data = f.read()
    out, version = sign_bin(data, "backup" if a.backup else "primary")
    with open(a.out, "wb") as f:
        f.write(out)
    print("signed %s (%s, %d B) -> %s" % (a.bin, version, len(data), a.out))


def cmd_sign_manifest(a):
    with open(a.manifest, "rb") as f:
        data = f.read()
    with open(a.out, "wb") as f:
        f.write(make_block(load_priv("backup" if a.backup else "primary"), DOMAIN_SD, manifest_tag(data), data))
    print("signed %s (%s) -> %s" % (a.manifest, manifest_tag(data), a.out))


def cmd_verify(a):
    if a.bin:
        with open(a.bin, "rb") as f:
            data = f.read()
        if not is_signed(data):
            sys.exit("unsigned")
        body = data[:-BLOCK]
        r = check_block(data[-BLOCK:], DOMAIN_OTA, image_desc(body)[0], body)
    else:
        with open(a.manifest, "rb") as f:
            data = f.read()
        with open(a.sig, "rb") as f:
            r = check_block(f.read(), DOMAIN_SD, manifest_tag(data), data)
    print(r)
    sys.exit(0 if r == "ok" else 1)


def gh(*args, capture=False):
    r = subprocess.run(["gh"] + list(args), check=True, capture_output=capture, text=capture)
    return r.stdout if capture else None


def cmd_release(a):
    """Sign a release CI already published: the OTA images get their block appended, the SD manifest gets a
    .sig, SHA256SUMS is rewritten for the changed files, then Pages is republished from it."""
    tag = a.tag
    tmp = tempfile.mkdtemp(prefix="fwsign-")
    try:
        names = gh("release", "view", tag, "-R", REPO, "--json", "assets", "-q", ".assets[].name", capture=True).split()
        otas = [n for n in names if n.endswith("-ota.bin")]
        mans = [n for n in names if n.endswith("-sd-manifest.txt")]
        if not otas or "SHA256SUMS" not in names:
            sys.exit("%s has no OTA image / SHA256SUMS" % tag)
        for n in otas + mans + ["SHA256SUMS"]:
            gh("release", "download", tag, "-R", REPO, "-p", n, "-D", tmp, "--clobber")
        sums = open(os.path.join(tmp, "SHA256SUMS")).read()
        upload = []
        for n in otas:
            p = os.path.join(tmp, n)
            data = open(p, "rb").read()
            if is_signed(data):
                body = data[:-BLOCK]
                if check_block(data[-BLOCK:], DOMAIN_OTA, image_desc(body)[0], body) != "ok":
                    sys.exit("%s carries a signature block that does not verify" % n)
                print("%s already signed" % n)
                continue
            # The OTA image must be the very build the release published (its SHA256SUMS line).
            if not re.search(r"^%s [ *]?%s$" % (hashlib.sha256(data).hexdigest(), re.escape(n)), sums, re.M):
                sys.exit("%s does not match SHA256SUMS" % n)
            out, version = sign_bin(data, "primary")
            if not version.startswith(tag.lstrip("v")):
                sys.exit("%s is version %s, not %s" % (n, version, tag))
            open(p, "wb").write(out)
            sums = re.sub(r"^[0-9a-f]{64}( [ *]?%s)$" % re.escape(n), hashlib.sha256(out).hexdigest() + r"\1", sums, flags=re.M)
            upload.append(p)
        for n in mans:
            p = os.path.join(tmp, n)
            data = open(p, "rb").read()
            sp = p[:-len(".txt")] + ".sig"
            open(sp, "wb").write(make_block(load_priv("primary"), DOMAIN_SD, manifest_tag(data), data))
            upload.append(sp)
        sp = os.path.join(tmp, "SHA256SUMS")
        open(sp, "w", newline="\n").write(sums)
        upload.append(sp)
        print("to upload:", ", ".join(os.path.basename(u) for u in upload))
        if a.dry_run:
            print("dry run: nothing uploaded")
            return
        gh("release", "upload", tag, "-R", REPO, "--clobber", *upload)
        gh("workflow", "run", "pages.yml", "-R", REPO)
        print("signed %s; Pages republish dispatched" % tag)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("keygen"); s.add_argument("--backup", action="store_true"); s.set_defaults(f=cmd_keygen)
    s = sub.add_parser("keys"); s.set_defaults(f=lambda a: gen_keys_c())
    s = sub.add_parser("fingerprint"); s.set_defaults(f=cmd_fingerprint)
    s = sub.add_parser("sign-bin"); s.add_argument("--bin", required=True); s.add_argument("--out", required=True)
    s.add_argument("--backup", action="store_true"); s.set_defaults(f=cmd_sign_bin)
    s = sub.add_parser("sign-manifest"); s.add_argument("--manifest", required=True); s.add_argument("--out", required=True)
    s.add_argument("--backup", action="store_true"); s.set_defaults(f=cmd_sign_manifest)
    s = sub.add_parser("verify"); s.add_argument("--bin"); s.add_argument("--manifest"); s.add_argument("--sig")
    s.set_defaults(f=cmd_verify)
    s = sub.add_parser("release"); s.add_argument("tag"); s.add_argument("--dry-run", action="store_true")
    s.set_defaults(f=cmd_release)
    a = ap.parse_args()
    a.f(a)


if __name__ == "__main__":
    main()
