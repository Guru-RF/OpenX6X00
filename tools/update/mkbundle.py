#!/usr/bin/env python3
"""Makes the update bundle the radio installs by itself (rootfs/usr/sbin/x6100-update): the files
the GUI and the web server put in the buildroot target, a manifest with a SHA-256 per file, and
an ECDSA P-256 / SHA-256 signature of the manifest (what the radio's OpenSSL 1.1.1 can check).

  python3 tools/update/mkbundle.py --target <buildroot target dir> \\
      --manifest <build/x6100-gui-*/install_manifest.txt> --version 0.2.0 \\
      --out OpenX6X00-v0.2.0-update.tar.gz [--key update-key.pem] [--notes "..."]

The key is the EC private key whose public half is rootfs/usr/share/x6100/update.pub
(openssl ecparam -name prime256v1 -genkey -noout -out key.pem); without it the bundle is
unsigned and a radio only takes it with /mnt/update/allow-unsigned present. Signing uses the
openssl command.

SPDX-License-Identifier: LGPL-2.1-or-later
Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
"""
import argparse
import hashlib
import io
import json
import os
import subprocess
import sys
import tarfile
import time

WEB_FILES = ["/usr/bin/x6100_webserver"]
WEB_DIRS = ["/usr/lib/python3.11/site-packages/x6100_webserver"]
SKIP_PREFIXES = ("/usr/include/",)       # the GUI package installs headers too


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", required=True)
    ap.add_argument("--manifest", required=True, help="the GUI's CMake install_manifest.txt")
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--key")
    ap.add_argument("--notes", default="")
    ap.add_argument("--model", default="x6100", choices=["x6100", "x6200"], help="the radio the bundle is built for")
    a = ap.parse_args()

    paths = set()
    with open(a.manifest) as f:
        for line in f:
            p = line.strip()
            p = "/" + os.path.normpath(p).lstrip("/")   # CMake writes "//etc/..." for the rootfs files
            if p != "/" and not p.startswith(SKIP_PREFIXES):
                paths.add(p)
    paths.update(WEB_FILES)
    for d in WEB_DIRS:
        for root, _, files in os.walk(os.path.join(a.target, d.lstrip("/"))):
            for n in files:
                paths.add("/" + os.path.relpath(os.path.join(root, n), a.target))

    entries = []
    for p in sorted(paths):
        src = os.path.join(a.target, p.lstrip("/"))
        if not os.path.isfile(src):
            sys.exit(f"missing in target: {p}")
        with open(src, "rb") as f:
            data = f.read()
        entries.append({"path": p, "sha256": hashlib.sha256(data).hexdigest(), "size": len(data),
                        "mode": "%o" % (os.stat(src).st_mode & 0o777)})
    manifest = {"product": "OpenX6X00", "model": a.model, "version": a.version.lstrip("v"),
                "built": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "notes": a.notes, "files": entries}
    mjson = json.dumps(manifest, indent=1, sort_keys=True).encode()

    sig = b""
    if a.key:
        r = subprocess.run(["openssl", "dgst", "-sha256", "-sign", a.key], input=mjson, capture_output=True)
        if r.returncode != 0:
            sys.exit("signing failed: " + r.stderr.decode())
        sig = r.stdout

    def add_bytes(tar, name, data, mode=0o644):
        ti = tarfile.TarInfo(name)
        ti.size, ti.mode, ti.mtime = len(data), mode, int(time.time())
        tar.addfile(ti, io.BytesIO(data))

    with tarfile.open(a.out, "w:gz") as tar:
        add_bytes(tar, "manifest.json", mjson)
        if sig:
            add_bytes(tar, "manifest.sig", sig)
        for e in entries:
            tar.add(os.path.join(a.target, e["path"].lstrip("/")), "files" + e["path"], recursive=False)
    total = sum(e["size"] for e in entries)
    print(f"{a.out}: {len(entries)} files, {total / 1e6:.1f} MB, {'signed' if sig else 'UNSIGNED'}")


if __name__ == "__main__":
    main()
