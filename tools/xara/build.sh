#!/bin/bash
# OpenX6X00 - XARA (the VARA-compatible HF modem, https://github.com/x0rro/xara) for the radios: one ARMv7
# binary for the X6100 and the X6200, built with the official Rust toolchain at the pinned version, from the
# pinned release's source and vendored crates (their SHA-256 checked), with our patches (patches/*.patch, in
# order) on top: the GUI's Winlink socket as its sound card and PTT (--audio x6x00, --ptt x6x00). Its
# `registered` feature is built in; it does nothing until xara is started with `registered` (the Winlink page's
# VARA licence tick box, off by default). See UPSTREAM.md.
#
#   CC=<the radios' gcc> tools/xara/build.sh    prints the binary's path on stdout, and nothing else
#   tools/xara/build.sh --host                  the same for this PC (x86_64 Linux), for tools/winlink's tests
#
# buildroot/local-build.sh runs it (in the build container and on the GitHub runner) with CC set to the buildroot
# tree's own cross gcc (glibc 2.36, the GUI's toolchain) and gives the path to the GUI's CMake (-DXARA_BINARY),
# which installs it as /usr/bin/xara and the doc/ beside it (XARA's licence, the crates' and the Rust standard
# library's notices) in /usr/share/x6100/licenses/xara. Everything is kept in $HOME/.cache/openx6x00: the toolchain
# (downloaded once, each part's SHA-256 checked), the release's two tarballs and the binary, under a name made of
# the XARA version, the Rust version, the target and the hash of the patches, of this script and of the linker. A
# second run with nothing changed prints that path and downloads nothing.
#
# Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
set -eo pipefail
exec 3>&1 1>&2          # stdout is the binary's path only: everything else goes to stderr

HERE=$(cd "$(dirname "$0")" && pwd)
XARA_VER=0.1.3
# The signed tag v0.1.3 (e32422a9) is this commit; the tarballs below are the release's, built from it
XARA_COMMIT=09a9201381b00499ada9d9c816bd05bb8f175705
XARA_URL=https://github.com/x0rro/xara/releases/download/v$XARA_VER
# The release's SHA256SUMS (2026-10-31)
XARA_SRC_SHA256=34f6fce5af2c32eca07359301ba2f58a49d1cc534b2a25b14c9606dcf84fc4cb
XARA_VENDOR_SHA256=dbac90dc109f2f6ab4d7840cf04e735f784fedd878d370f2d501b0fde8a8ae80
# The Rust XARA's own releases are built with (its .rust-version), 2026-10-15; the hashes are
# static.rust-lang.org's (<file>.sha256)
RUST_VER=1.99.1
RUST_URL=https://static.rust-lang.org/dist
RUST_PARTS=(
    "rustc-$RUST_VER-x86_64-unknown-linux-gnu 2ff05ff9c08a90861e7f6e4065b40a8b87463b4576186fd613871c3e37f2091f"
    "cargo-$RUST_VER-x86_64-unknown-linux-gnu 53f3521949404d595efb06a55c12c5658d4ed1c75566f33d3d514eaebe70bd8a"
    "rust-std-$RUST_VER-x86_64-unknown-linux-gnu 109b074225bf5b9ea3820890876bfdf4ed161adbfaf94b2b4b5984c85ee5d958"
    "rust-std-$RUST_VER-armv7-unknown-linux-gnueabihf 53fdf6581a24f510def5173813a0d23518a8cde547b9fbf4876499f19feda18c"
)
GLIBC_MAX=2.36          # the radios' (buildroot 2023.02, both trees)

HOST=0
case "$1" in
    "") ;;
    --host) HOST=1 ;;
    *) echo "usage: [CC=<cross gcc>] tools/xara/build.sh [--host]"; exit 2 ;;
esac
[ "$(uname -m)" = x86_64 ] || { echo "tools/xara/build.sh: the pinned Rust toolchain is for x86_64 hosts, not $(uname -m)"; exit 1; }

if [ $HOST = 1 ]; then
    TARGET=x86_64-unknown-linux-gnu
    LINKER=${CC:-cc}
else
    TARGET=armv7-unknown-linux-gnueabihf
    LINKER=${CC:-}
    [ -n "$LINKER" ] || { echo "tools/xara/build.sh: CC must name the radios' gcc (arm-buildroot-linux-gnueabihf-gcc)"; exit 1; }
    case "$("$LINKER" -dumpmachine)" in
        arm*-linux-gnueabihf) ;;
        *) echo "tools/xara/build.sh: $LINKER is not a gcc for arm-linux-gnueabihf"; exit 1 ;;
    esac
fi
command -v "$LINKER" > /dev/null || { echo "tools/xara/build.sh: no $LINKER"; exit 1; }

CACHE=$HOME/.cache/openx6x00
RUST=$CACHE/rust-$RUST_VER
DL=$CACHE/xara/dl
PATCHES=("$HERE"/patches/*.patch)
[ -e "${PATCHES[0]}" ] || PATCHES=()
# The linker is part of the result: its version and target, and its C library's
KEY=$( (cat "${PATCHES[@]}" "$HERE/build.sh"; "$LINKER" --version | head -1; "$LINKER" -dumpmachine
        cat "$("$LINKER" -print-file-name=libc.so.6)" 2> /dev/null || true) | sha256sum | cut -c1-12)
OUT=$CACHE/xara/xara-$XARA_VER-rust$RUST_VER-$TARGET-$KEY/xara

if [ -x "$OUT" ] && [ -f "$(dirname "$OUT")/doc/LICENSE" ]; then
    echo "== XARA $XARA_VER (Rust $RUST_VER, $TARGET, patches $KEY): built already"
    echo "$OUT" >&3
    exit 0
fi
mkdir -p "$DL"

fetch() {   # URL FILE SHA256: downloaded once, kept only with the pinned hash
    [ -f "$2" ] && echo "$3  $2" | sha256sum -c --quiet - 2> /dev/null && return 0
    echo "== $1"
    if command -v curl > /dev/null; then
        curl -fsSL --retry 3 -o "$2.tmp.$$" "$1"
    else
        wget -q -O "$2.tmp.$$" "$1"
    fi
    echo "$3  $2.tmp.$$" | sha256sum -c --quiet - \
        || { echo "tools/xara/build.sh: $1 does not have the pinned SHA-256"; rm -f "$2.tmp.$$"; exit 1; }
    mv -f "$2.tmp.$$" "$2"
}

# The toolchain: rustc, cargo and the two standard libraries (this PC's for build scripts, the target's), each
# part unpacked into one directory, as rustup would
if [ ! -x "$RUST/bin/cargo" ]; then
    T=$(mktemp -d "$CACHE/rust-$RUST_VER.tmp.XXXXXX")
    trap 'rm -rf "$T"' EXIT     # a download or unpack that fails leaves nothing in the cache
    mkdir "$T/root"
    for p in "${RUST_PARTS[@]}"; do
        name=${p% *}
        fetch "$RUST_URL/$name.tar.xz" "$DL/$name.tar.xz" "${p#* }"
        tar xJf "$DL/$name.tar.xz" -C "$T"
        for c in $(cat "$T/$name/components"); do
            (cd "$T/$name/$c" && tar cf - --exclude=manifest.in .) | tar xf - -C "$T/root"
        done
        rm -rf "${T:?}/$name"
    done
    # Moved into place whole (a build next to this one may have done it in the meantime)
    [ -x "$RUST/bin/cargo" ] || { rm -rf "$RUST"; mv "$T/root" "$RUST"; }
    rm -rf "$T"
    trap - EXIT
fi
"$RUST/bin/rustc" --version | grep -q "^rustc $RUST_VER " \
    && "$RUST/bin/cargo" --version | grep -q "^cargo $RUST_VER " \
    && [ -d "$RUST/lib/rustlib/$TARGET/lib" ] \
    || { echo "tools/xara/build.sh: $RUST is not Rust $RUST_VER with $TARGET"; exit 1; }

fetch "$XARA_URL/xara-$XARA_VER.tar.xz" "$DL/xara-$XARA_VER.tar.xz" $XARA_SRC_SHA256
fetch "$XARA_URL/xara-$XARA_VER-vendor.tar.xz" "$DL/xara-$XARA_VER-vendor.tar.xz" $XARA_VENDOR_SHA256

SRC=$(mktemp -d "$CACHE/xara/src.XXXXXX")
trap 'rm -rf "$SRC"' EXIT
echo "== XARA $XARA_VER ($XARA_COMMIT)"
# The source, and the crates it depends on in vendor/ with the .cargo/config.toml that points cargo at them
tar xJf "$DL/xara-$XARA_VER.tar.xz" -C "$SRC"
tar xJf "$DL/xara-$XARA_VER-vendor.tar.xz" -C "$SRC"
cd "$SRC/xara-$XARA_VER"
for p in "${PATCHES[@]}"; do
    echo "   $(basename "$p")"
    git apply "$p"
done

# Rust as pinned and nothing else: no rustup, no flags, wrappers or directories from the environment
for v in $(compgen -e); do
    case $v in CARGO_*|RUSTC*|RUSTDOC*|RUSTFLAGS|RUSTUP_*) unset "$v" ;; esac
done
export PATH=$RUST/bin:$PATH RUSTC=$RUST/bin/rustc
export CARGO_HOME=$CACHE/xara/cargo CARGO_TARGET_DIR=$SRC/target
T_VAR=CARGO_TARGET_$(echo $TARGET | tr a-z- A-Z_)_LINKER
export "$T_VAR=$LINKER"
# The binary names neither this build's directory nor the cache's; no symbols (6 MB less)
export RUSTFLAGS="--remap-path-prefix=$SRC=/build --remap-path-prefix=$CARGO_HOME=/cargo"
export CARGO_PROFILE_RELEASE_STRIP=symbols

echo "== $TARGET binary (offline: the vendored crates, Cargo.lock as it is)"
# No PulseAudio or ALSA (the radio's audio is the GUI's); the socket backend (x6x00) and registered
cargo build --frozen --release -p xara --bin xara --target $TARGET --no-default-features --features registered,x6x00
BIN=$SRC/target/$TARGET/release/xara

# The notices that go with the binary: XARA's licence, each crate compiled in (cargo tree, the vendored copy's MIT
# text: they are all MIT, some also Apache-2.0, Zlib or 0BSD, at the user's choice) and the Rust standard library's
DOC=$SRC/doc
mkdir "$DOC"
cp LICENSE "$DOC/LICENSE"
CRATES=$(cargo tree --frozen -e normal --target $TARGET -p xara --no-default-features --features registered,x6x00 \
             --prefix none | sed 's/ (\*)$//' | grep -v '^xara' | sort -u)
{
    echo "XARA $XARA_VER, as built for OpenX6X00 (tools/xara), contains these crates, each used here under its"
    echo "MIT licence, and the Rust standard library of Rust $RUST_VER (Apache-2.0 OR MIT; its notices in"
    echo "rust-std-COPYRIGHT.html.gz)."
    while read -r name ver; do
        d=vendor/$name
        lic=$(sed -n 's/^license = "\(.*\)"$/\1/p' "$d/Cargo.toml")
        f=""
        for c in LICENSE-MIT LICENSE-MIT.md LICENSE; do
            [ -f "$d/$c" ] && { f=$d/$c; break; }
        done
        [ -n "$f" ] || { echo "tools/xara/build.sh: no MIT licence text in $d" >&2; exit 1; }
        printf '\n==== %s %s (%s)\n\n' "$name" "$ver" "$lic"
        cat "$f"
    done <<< "$CRATES"
} > "$DOC/THIRD-PARTY"
gzip -9 -n -c "$RUST/share/doc/rust/COPYRIGHT-library.html" > "$DOC/rust-std-COPYRIGHT.html.gz"
echo "== notices: LICENSE, THIRD-PARTY ($(echo "$CRATES" | wc -l) crates), rust-std-COPYRIGHT.html.gz"

if [ $HOST = 0 ]; then
    READELF=${LINKER%gcc}readelf
    command -v "$READELF" > /dev/null || READELF=readelf
    # A 32-bit little-endian ELF (7f 'E' 'L' 'F' 01 01) for ARM (e_machine 0x28, bytes 18-19)
    ELF=$(od -An -tx1 -N20 "$BIN" | tr -d ' \n')
    [ "${ELF:0:12}" = 7f454c460101 ] && [ "${ELF:36:4}" = 2800 ] \
        || { echo "tools/xara/build.sh: the result is not a 32-bit ARM ELF"; exit 1; }
    # What the radios have: the C library only, no PulseAudio or ALSA
    NEEDED=$("$READELF" -dW "$BIN" | sed -n 's/.*(NEEDED).*\[\(.*\)\]$/\1/p' | sort | tr '\n' ' ')
    for n in $NEEDED; do
        case $n in libc.so.6|libm.so.6|libgcc_s.so.1|ld-linux-armhf.so.3) ;;
            *) echo "tools/xara/build.sh: the binary needs $n, which the radios may not have"; exit 1 ;;
        esac
    done
    GLIBC=$("$READELF" --dyn-syms -W "$BIN" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)
    [ "$(printf '%s\n%s\n' "$GLIBC" $GLIBC_MAX | sort -V | tail -1)" = $GLIBC_MAX ] \
        || { echo "tools/xara/build.sh: the binary needs glibc $GLIBC, the radios have $GLIBC_MAX"; exit 1; }
    echo "== needs $NEEDED(glibc $GLIBC at most)"
fi
mkdir -p "$(dirname "$OUT")"
rm -rf "$(dirname "$OUT")/doc"
mv "$DOC" "$(dirname "$OUT")/doc"
mv -f "$BIN" "$OUT.tmp.$$"
mv -f "$OUT.tmp.$$" "$OUT"
echo "== $(wc -c < "$OUT") bytes, sha256 $(sha256sum "$OUT" | cut -d' ' -f1)"
echo "$OUT" >&3
