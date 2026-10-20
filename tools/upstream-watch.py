#!/usr/bin/env python3
"""Tracks a release of the closed X6100 firmware this project was forked from.

For one release (the latest by default) it downloads the GPL sources tarball and the SD
image, then writes <tag>.md in its cache (--out for elsewhere; the reports stay out of the repository) with:

  1. the release notes;
  2. the buildroot external tree of the release (kernel config, patches, overlay) diffed
     against the AetherX6100Buildroot tree next to this repository;
  3. the helper sources of the release (the JS8, JTTY and WSPR modems) diffed against the
     files of the same name in src/;
  4. what the closed GUI binary says: its text strings that are not in our own GUI binary
     (a built one, or the previous release's binary when there is one), as a feature list;
  5. the scripts, init entries and web pages of the release image against ours.

The patches themselves land in the cache next to the downloads (--cache); the report names
them. Reading the strings of a binary is all this does with it: no disassembly.

Needs python3, tar, unzip, debugfs (e2fsprogs), strings (binutils) and diff. No modules.

  python3 -I tools/upstream-watch.py                 # the latest release
  python3 -I tools/upstream-watch.py v0.129          # that one
  python3 -I tools/upstream-watch.py v0.129 --assets ~/Downloads   # files already downloaded

SPDX-License-Identifier: LGPL-2.1-or-later
Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from datetime import datetime, timezone

REPO_API = "https://api.github.com/repos/IU1WXY/X6100-IU1WXY/releases"
USER_AGENT = "OpenX6X00 upstream-watch (+https://github.com/Guru-RF/OpenX6X00)"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# The helper directories of the release and where their files live here
HELPER_DIRS = {"helpers/src/js8": "src/js8", "helpers/src/jtty": "src", "helpers/src/wspr": "src/wspr",
               "helpers/src": "src"}
# Strings worth listing: UI text, not symbols. A capital, a space, mostly letters.
UI_STRING = re.compile(r"^[A-Z][A-Za-z0-9,.:;'()/%+\-?!<>{}]*( [A-Za-z0-9,.:;'()/%+\-?!<>{}]+)+$")
NOISE = re.compile(r"^(GLIBC_|GCC_|CXXABI_|_Z|lv_|nm_|pa_|g_|snd_|sqlite3_|png_|jpeg_|gps_|udev_|dbus_)")


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


def fetch_json(url):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def download(url, dest):
    if os.path.exists(dest) and os.path.getsize(dest) > 0:
        return
    print(f"downloading {os.path.basename(dest)}")
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    tmp = dest + ".part"
    with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f, 1 << 20)
    os.replace(tmp, dest)


def release_info(tag):
    rel = fetch_json(REPO_API + ("/latest" if not tag else f"/tags/{tag}"))
    assets = {a["name"]: a for a in rel.get("assets", [])}
    src = next((a for n, a in assets.items() if n.endswith("gpl-sources.tar.gz")), None)
    img = next((a for n, a in assets.items() if n.endswith("sdcard.img.zip")), None)
    return rel, src, img


def extract_sources(tgz, into):
    if os.path.isdir(into):
        return into
    with tarfile.open(tgz) as t:
        t.extractall(into, filter="data")
    return into


def extract_rootfs(imgzip, into):
    """The ext4 root partition of the SD image, dumped with debugfs (no root needed)."""
    ext4 = os.path.join(into, "rootfs.ext4")
    if os.path.exists(ext4):
        return ext4
    os.makedirs(into, exist_ok=True)
    with zipfile.ZipFile(imgzip) as z:
        name = next(n for n in z.namelist() if n.endswith(".img"))
        img = os.path.join(into, "sdcard.img")
        if not os.path.exists(img):
            z.extract(name, into)
            os.replace(os.path.join(into, name), img)
    # partition 2 from the MBR: start sector and size at offset 446 + 16
    with open(img, "rb") as f:
        f.seek(446 + 16)
        e = f.read(16)
    start, size = int.from_bytes(e[8:12], "little"), int.from_bytes(e[12:16], "little")
    with open(img, "rb") as f, open(ext4, "wb") as out:
        f.seek(start * 512)
        left = size * 512
        while left > 0:
            chunk = f.read(min(left, 1 << 20))
            if not chunk:
                break
            out.write(chunk)
            left -= len(chunk)
    os.remove(img)
    return ext4


def rootfs_ls(ext4, path):
    out = subprocess.run(["debugfs", "-R", f"ls -p {path}", ext4], capture_output=True, text=True).stdout
    names = []
    for line in out.splitlines():
        parts = line.split("/")
        if len(parts) > 5 and parts[5] not in (".", "..", ""):
            names.append(parts[5])
    return sorted(names)


def rootfs_dump(ext4, path, dest):
    if not os.path.exists(dest):
        subprocess.run(["debugfs", "-R", f"dump {path} {dest}", ext4], capture_output=True, text=True)
    return dest if os.path.exists(dest) else None


def strings_of(binary):
    if not binary or not os.path.exists(binary):
        return set()
    return set(run(["strings", "-n", "6", binary]).splitlines())


def diff_dirs(a, b, out_patch):
    """Unified diff of two trees; returns (added, removed, changed) lists and writes the patch."""
    p = subprocess.run(["diff", "-ruN", "--no-dereference", "-x", ".git", a, b], capture_output=True, text=True,
                       errors="replace")
    with open(out_patch, "w") as f:
        f.write(p.stdout)
    added, removed, changed = [], [], []
    cur = None
    for line in p.stdout.splitlines():
        if line.startswith("diff -ruN"):
            cur = line.split(" ")[-1]
            rel = os.path.relpath(cur, b)
            changed.append(rel)
        elif line.startswith("--- ") and cur:
            if "\t1970-01-01" in line or line.endswith("1970-01-01 01:00:00.000000000 +0100"):
                added.append(changed.pop())
        elif line.startswith("+++ ") and cur:
            if "\t1970-01-01" in line:
                removed.append(changed.pop())
    return added, removed, changed


def diff_file(a, b):
    p = subprocess.run(["diff", "-u", a, b], capture_output=True, text=True, errors="replace")
    plus = sum(1 for l in p.stdout.splitlines() if l.startswith("+") and not l.startswith("+++"))
    minus = sum(1 for l in p.stdout.splitlines() if l.startswith("-") and not l.startswith("---"))
    return p.returncode, plus, minus, p.stdout


def find_ours(name, hint):
    """A file of this name in our tree: the hinted directory first, then anywhere under src/."""
    cand = os.path.join(ROOT, hint, name)
    if os.path.exists(cand):
        return cand
    for dp, _, fs in os.walk(os.path.join(ROOT, "src")):
        if name in fs:
            return os.path.join(dp, name)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("tag", nargs="?", help="release tag (vX.Y), default the latest")
    ap.add_argument("--cache", default=os.path.expanduser("~/.cache/openx6x00-upstream"), help="downloads and patches")
    ap.add_argument("--assets", help="a directory that already holds the release's files")
    ap.add_argument("--buildroot", default=os.path.join(os.path.dirname(ROOT), "AetherX6100Buildroot"),
                    help="our AetherX6100Buildroot checkout (its br2_external is compared)")
    ap.add_argument("--ours-gui", help="our built x6100_gui (default: the buildroot target's)")
    ap.add_argument("--out", help="where the report goes (default: the cache, next to the downloads)")
    args = ap.parse_args()

    rel, src_asset, img_asset = release_info(args.tag)
    tag = rel["tag_name"]
    cache = os.path.join(args.cache, tag)
    os.makedirs(cache, exist_ok=True)
    print(f"release {tag} of {rel.get('published_at', '')[:10]}")

    def asset_path(a):
        if args.assets and os.path.exists(os.path.join(args.assets, a["name"])):
            return os.path.join(args.assets, a["name"])
        dest = os.path.join(cache, a["name"])
        download(a["browser_download_url"], dest)
        return dest

    report = [f"# Upstream release {tag}", "",
              f"X6100 IU1WXY {tag}, published {rel.get('published_at', '')[:10]}, "
              f"[release page]({rel.get('html_url', '')}). Report made {datetime.now(timezone.utc).date()} by "
              f"`tools/upstream-watch.py`; the patches named below are in `{cache}`.", ""]
    if rel.get("assets"):
        report += ["| Asset | Size |", "|---|---|"]
        report += [f"| {a['name']} | {a['size'] / 1e6:.1f} MB |" for a in rel["assets"]]
        report.append("")

    # 1. release notes
    report += ["## Release notes", "", "> " + (rel.get("body") or "(none)").replace("\r", "").replace("\n", "\n> "), ""]

    # 2. buildroot external tree
    if src_asset:
        tgz = asset_path(src_asset)
        srcdir = extract_sources(tgz, os.path.join(cache, "gpl-sources"))
        top = next((os.path.join(srcdir, d) for d in os.listdir(srcdir) if os.path.isdir(os.path.join(srcdir, d))), srcdir)
        theirs_ext = os.path.join(top, "buildroot", "br2_external")
        ours_ext = os.path.join(args.buildroot, "br2_external")
        report += ["## Buildroot external tree", ""]
        if os.path.isdir(theirs_ext) and os.path.isdir(ours_ext):
            patch = os.path.join(cache, "br2_external.patch")
            added, removed, changed = diff_dirs(ours_ext, theirs_ext, patch)
            report.append(f"Their `buildroot/br2_external` against `{ours_ext}`: "
                          f"{len(added)} files only theirs, {len(removed)} only ours, {len(changed)} differ. "
                          f"Patch: `{patch}` (ours -> theirs).")
            report.append("")
            for title, items in (("Only in the release", added), ("Only in ours", removed), ("Differ", changed)):
                if items:
                    report += [f"**{title}:**", ""] + [f"- `{i}`" for i in items[:80]]
                    if len(items) > 80:
                        report.append(f"- ... {len(items) - 80} more")
                    report.append("")
        else:
            report += ["Not compared: " + ("no br2_external in the release" if not os.path.isdir(theirs_ext)
                                            else f"no buildroot checkout at {args.buildroot}"), ""]

        # 3. helpers
        report += ["## Helper sources (the JS8, JTTY and WSPR modems)", ""]
        helpers = os.path.join(top, "helpers")
        rows = []
        if os.path.isdir(helpers):
            for dp, _, fs in os.walk(os.path.join(helpers, "src")):
                for f in sorted(fs):
                    if not f.endswith((".c", ".cpp", ".h", ".hpp")):
                        continue
                    theirs = os.path.join(dp, f)
                    rel_dir = os.path.relpath(dp, top)
                    hint = HELPER_DIRS.get(rel_dir, "src")
                    ours = find_ours(f, hint)
                    lic = ""
                    with open(theirs, errors="replace") as fh:
                        m = re.search(r"SPDX-License-Identifier: (\S+)", fh.read(2000))
                        lic = m.group(1) if m else "?"
                    if not ours:
                        rows.append((os.path.join(rel_dir, f), lic, "not in our tree", ""))
                        continue
                    rc, plus, minus, text = diff_file(ours, theirs)
                    if rc == 0:
                        rows.append((os.path.join(rel_dir, f), lic, "same", os.path.relpath(ours, ROOT)))
                    else:
                        pf = os.path.join(cache, "helpers", os.path.relpath(theirs, helpers) + ".patch")
                        os.makedirs(os.path.dirname(pf), exist_ok=True)
                        with open(pf, "w") as fh:
                            fh.write(text)
                        rows.append((os.path.join(rel_dir, f), lic, f"differs (+{plus} -{minus})", os.path.relpath(ours, ROOT)))
            report += ["| Their file | Licence | Against ours | Our file |", "|---|---|---|---|"]
            report += [f"| `{a}` | {b} | {c} | {('`' + d + '`') if d else ''} |" for a, b, c, d in rows]
            n_diff = sum(1 for r in rows if r[2].startswith("differs"))
            report += ["", f"{len(rows)} files, {n_diff} differ; the patches (ours -> theirs) are under `{cache}/helpers/`. "
                       "Only the LGPL files may go into the GUI; the GPL-3 ones belong in a separate program.", ""]
        else:
            report += ["No helpers/ in the release.", ""]
        sources_txt = os.path.join(top, "SOURCES.txt")
        if os.path.exists(sources_txt):
            with open(sources_txt, errors="replace") as fh:
                pk = [l.split()[0:2] for l in fh if re.match(r"^[a-z0-9_.-]+ \S+ ", l)]
            report += [f"The release lists {len(pk)} upstream packages in SOURCES.txt.", ""]

    # 4. and 5. the image
    if img_asset:
        imgzip = asset_path(img_asset)
        ext4 = extract_rootfs(imgzip, os.path.join(cache, "image"))
        gui = rootfs_dump(ext4, "/usr/sbin/x6100_gui", os.path.join(cache, "x6100_gui.bin"))
        theirs = strings_of(gui)
        with open(os.path.join(cache, "x6100_gui.strings"), "w") as fh:
            fh.write("\n".join(sorted(theirs)))
        # the reference: our built GUI, else the previous cached release
        ours_gui = args.ours_gui or os.path.join(args.buildroot, "build", "target", "usr", "sbin", "x6100_gui")
        ref_name = f"our GUI `{os.path.relpath(ours_gui, ROOT) if ours_gui.startswith(ROOT) else ours_gui}`"
        if not os.path.exists(ours_gui):
            prev = sorted((d for d in os.listdir(args.cache) if d != tag and os.path.exists(os.path.join(args.cache, d, "x6100_gui.strings"))),
                          key=lambda v: [int(x) for x in re.findall(r"\d+", v)])
            if prev:
                with open(os.path.join(args.cache, prev[-1], "x6100_gui.strings")) as fh:
                    ours_strings = set(fh.read().splitlines())
                ref_name = f"the {prev[-1]} binary"
            else:
                ours_strings = set()
                ref_name = "nothing (no reference binary found)"
        else:
            ours_strings = strings_of(ours_gui)
        new = sorted(s for s in theirs - ours_strings if UI_STRING.match(s) and not NOISE.match(s))
        gone = sorted(s for s in ours_strings - theirs if UI_STRING.match(s) and not NOISE.match(s))
        report += ["## What the GUI binary says", "",
                   f"Text strings of their `/usr/sbin/x6100_gui` ({os.path.getsize(gui) / 1e6:.1f} MB) that {ref_name} does not have: "
                   f"{len(new)} lines that read as interface text (of {len(theirs - ours_strings)} new strings in all; the full "
                   f"list is `{cache}/x6100_gui.strings`). What the release added, in its own words:", ""]
        report += [f"- {s}" for s in new[:400]]
        if len(new) > 400:
            report.append(f"- ... {len(new) - 400} more")
        report += ["", f"Strings of ours that theirs lacks ({len(gone)}): what was removed or renamed there, or is only ours.", ""]
        report += [f"- {s}" for s in gone[:150]]
        if len(gone) > 150:
            report.append(f"- ... {len(gone) - 150} more")
        report.append("")

        report += ["## Scripts, services and web pages of the image", ""]
        # our image's root (a buildroot build next door) when there is one, else only our overlay
        target = os.path.join(args.buildroot, "build", "target")
        ours_sbin = os.path.join(target, "usr", "sbin") if os.path.isdir(target) else os.path.join(ROOT, "rootfs", "usr", "sbin")
        ours_init = os.path.join(target, "etc", "init.d") if os.path.isdir(target) else os.path.join(ROOT, "rootfs", "etc", "init.d")
        if os.path.isdir(target):
            report += [f"Ours is the built image at `{target}`.", ""]
        else:
            report += ["Ours is only this repository's rootfs overlay (no buildroot build next door): the lists below "
                       "name every script of their image, ours would show the same buildroot ones.", ""]
        for title, path, ours_dir in (("Helper programs and scripts (`/usr/sbin/x6100*`)", "/usr/sbin", ours_sbin),
                                      ("Init scripts (`/etc/init.d`)", "/etc/init.d", ours_init),
                                      ("Web pages (`x6100_webserver/views`)", "/usr/lib/python3.11/site-packages/x6100_webserver/views",
                                       os.path.join(ROOT, "webserver", "src", "x6100_webserver", "views"))):
            theirs_l = [n for n in rootfs_ls(ext4, path) if path != "/usr/sbin" or n.startswith("x6100")]
            ours_l = sorted(os.listdir(ours_dir)) if os.path.isdir(ours_dir) else []
            if path == "/usr/sbin":
                ours_l = [n for n in ours_l if n.startswith("x6100")]
                if not os.path.isdir(target):
                    ours_l += ["x6100_gui", "x6100-bootanim"]
            only_t = sorted(set(theirs_l) - set(ours_l))
            only_o = sorted(set(ours_l) - set(theirs_l))
            report += [f"**{title}**: only in the release: {', '.join(f'`{n}`' for n in only_t) or 'none'}; "
                       f"only in ours: {', '.join(f'`{n}`' for n in only_o) or 'none'}.", ""]
        # their scripts that we also have: a line diff
        rows = []
        for n in rootfs_ls(ext4, "/usr/sbin"):
            ours = os.path.join(ROOT, "rootfs", "usr", "sbin", n)
            if n.startswith("x6100") and os.path.exists(ours):
                t = rootfs_dump(ext4, f"/usr/sbin/{n}", os.path.join(cache, "image", n))
                if t:
                    rc, plus, minus, text = diff_file(ours, t)
                    if rc:
                        with open(os.path.join(cache, "image", n + ".patch"), "w") as fh:
                            fh.write(text)
                    rows.append(f"- `{n}`: " + ("same" if rc == 0 else f"differs (+{plus} -{minus}), patch in `{cache}/image/`"))
        if rows:
            report += ["Scripts we both have:", ""] + rows + [""]

    out_dir = args.out or args.cache
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, f"{tag}.md")
    with open(out, "w") as fh:
        fh.write("\n".join(report) + "\n")
    print(f"report: {out}")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as e:
        sys.exit(f"{e.cmd[0]} failed: {e.stderr.strip()}")
    except (OSError, urllib.error.URLError) as e:
        sys.exit(str(e))
