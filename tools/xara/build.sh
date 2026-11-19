#!/bin/bash
# OpenX6X00 - XARA (the VARA-compatible HF modem by x0rro, continued by NR5P: https://github.com/NR5P/xara) for the
# radios: one ARMv7 binary for the X6100 and the X6200, built with the official Rust toolchain at the pinned version,
# from the pinned commit's source (GitHub's tarball of it) and the crates its Cargo.lock names (from crates.io), each
# checked against its SHA-256, with our patches (patches/*.patch, in order) on top: xara 0.1.3's modem core and
# resampler, the GUI's Winlink socket as its sound card and PTT (--audio x6x00, --ptt x6x00), its CW ID when Pat asks
# for one (CWID ON) whatever the callsign, a call its host cancels given up at once, and a BUSY that reads a radio's
# sloped passband as clear (and says why it came on). Its `registered` feature is built in; it does nothing until
# xara is started with `registered` (the Winlink page's VARA licence tick box, off by default). See UPSTREAM.md.
#
#   CC=<the radios' gcc> tools/xara/build.sh    prints the binary's path on stdout, and nothing else
#   tools/xara/build.sh --host                  the same for this PC (x86_64 Linux), for tools/winlink's tests
#   tools/xara/build.sh --test                  XARA's own tests (cargo test) with our patches, on this PC, offline
#                                               once the cache has it all: tools/xara's `make test`
#
# buildroot/local-build.sh runs it (in the build container and on the GitHub runner) with CC set to the buildroot
# tree's own cross gcc (glibc 2.36, the GUI's toolchain) and gives the path to the GUI's CMake (-DXARA_BINARY),
# which installs it as /usr/bin/xara and the doc/ beside it (XARA's licence, the crates' and the Rust standard
# library's notices) in /usr/share/x6100/licenses/xara. Everything is kept in $HOME/.cache/openx6x00: the toolchain
# (downloaded once, each part's SHA-256 checked), the source tarball, the crates (cargo's own cache) and the binary,
# under a name made of the XARA version and commit, the Rust version, the target and the hash of the patches, of
# this script and of the linker. A second run with nothing changed prints that path and downloads nothing.
#
# Copyright (c) 2026 Joeri Van Dooren ON6URE / Guru-RF
set -eo pipefail
exec 3>&1 1>&2          # stdout is the binary's path only: everything else goes to stderr

HERE=$(cd "$(dirname "$0")" && pwd)
# NR5P's main on 9 November 2026 (x0rro discontinued XARA on 8 November), the version its Cargo.toml says
XARA_VER=0.1.2
XARA_COMMIT=bd8300a36e4f76fae7d561335bf08649474bdc6d
# GitHub's tarball of the commit (git archive). The SHA-256 is the tar's in it, which gzip's own bytes do not
# change, and which any clone gives back: git archive --format=tar --prefix=xara-$XARA_COMMIT/ $XARA_COMMIT
XARA_URL=https://codeload.github.com/NR5P/xara/tar.gz/$XARA_COMMIT
XARA_TAR_SHA256=3aa6bc2cf8e175b373ded6bde1554b0adf98677bc2cd395c5df803c16c67b520
# Rust 1.99.1 (2026-10-15), what x0rro's last release (0.1.3) was built with (its .rust-version); NR5P's needs 1.85
# or newer. The hashes are static.rust-lang.org's (<file>.sha256)
RUST_VER=1.99.1
RUST_URL=https://static.rust-lang.org/dist
RUST_PARTS=(
    "rustc-$RUST_VER-x86_64-unknown-linux-gnu 2ff05ff9c08a90861e7f6e4065b40a8b87463b4576186fd613871c3e37f2091f"
    "cargo-$RUST_VER-x86_64-unknown-linux-gnu 53f3521949404d595efb06a55c12c5658d4ed1c75566f33d3d514eaebe70bd8a"
    "rust-std-$RUST_VER-x86_64-unknown-linux-gnu 109b074225bf5b9ea3820890876bfdf4ed161adbfaf94b2b4b5984c85ee5d958"
    "rust-std-$RUST_VER-armv7-unknown-linux-gnueabihf 53fdf6581a24f510def5173813a0d23518a8cde547b9fbf4876499f19feda18c"
)
GLIBC_MAX=2.36          # the radios' (buildroot 2023.02, both trees)

HOST=0 TEST=0
case "$1" in
    "") ;;
    --host) HOST=1 ;;
    --test) HOST=1 TEST=1 ;;
    *) echo "usage: [CC=<cross gcc>] tools/xara/build.sh [--host|--test]"; exit 2 ;;
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
OUT=$CACHE/xara/xara-$XARA_VER-${XARA_COMMIT:0:12}-rust$RUST_VER-$TARGET-$KEY/xara

if [ $TEST = 0 ] && [ -x "$OUT" ] && [ -f "$(dirname "$OUT")/doc/LICENSE" ]; then
    echo "== XARA $XARA_VER, NR5P's ${XARA_COMMIT:0:12} (Rust $RUST_VER, $TARGET, patches $KEY): built already"
    echo "$OUT" >&3
    exit 0
fi
mkdir -p "$DL"

sha() {     # FILE [tar]: its SHA-256, or with tar the SHA-256 of the tar in the gzip FILE
    if [ "$2" = tar ]; then gunzip -c "$1" 2> /dev/null; else cat "$1"; fi | sha256sum | cut -d' ' -f1
}

fetch() {   # URL FILE SHA256 [tar]: downloaded once, kept only with the pinned hash (with tar: of the tar in it)
    [ -f "$2" ] && [ "$(sha "$2" $4)" = "$3" ] && return 0
    echo "== $1"
    if command -v curl > /dev/null; then
        curl -fsSL --retry 3 -o "$2.tmp.$$" "$1"
    else
        wget -q -O "$2.tmp.$$" "$1"
    fi
    [ "$(sha "$2.tmp.$$" $4)" = "$3" ] \
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

fetch "$XARA_URL" "$DL/xara-$XARA_COMMIT.tar.gz" $XARA_TAR_SHA256 tar

SRC=$(mktemp -d "$CACHE/xara/src.XXXXXX")
trap 'rm -rf "$SRC"' EXIT
echo "== XARA $XARA_VER, NR5P's commit $XARA_COMMIT"
tar xzf "$DL/xara-$XARA_COMMIT.tar.gz" -C "$SRC"
cd "$SRC/xara-$XARA_COMMIT"
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
# Nor from a cargo config file: cargo reads one in each parent of the directory it runs in ($HOME/.cargo/config.toml
# too, the cache being in $HOME; in the container /work/.brhome/.cargo and /work/.cargo) and in its CARGO_HOME, and
# one there (a [source] mirror, [build] rustflags or a wrapper, [net] or [http]) would change what is fetched or
# built. The source's own would be pinned with it; NR5P's has none
CONFIGS=("$CARGO_HOME/config.toml" "$CARGO_HOME/config")
d=$(cd .. && pwd -P)
while :; do
    CONFIGS+=("${d%/}/.cargo/config.toml" "${d%/}/.cargo/config")
    [ "$d" = / ] && break
    d=$(dirname "$d")
done
for f in "${CONFIGS[@]}"; do
    [ ! -e "$f" ] \
        || { echo "tools/xara/build.sh: cargo would read $f, which can change what it fetches or builds: move it aside"; exit 1; }
done

# The crates Cargo.lock names for this target, from crates.io into cargo's cache (once): cargo takes each one only
# with the SHA-256 the lock gives it, and the lock is the pinned source's
echo "== the crates (Cargo.lock as it is, each checked against its SHA-256 there)"
cargo fetch --locked --target $TARGET

if [ $TEST = 1 ]; then
    # The crates our patches change, and the xara crate as the radios have it (no PulseAudio or ALSA)
    echo "== cargo test (offline)"
    cargo test --frozen --release -p xara-arq -p xara-phy -p xara-host
    cargo test --frozen --release -p xara --no-default-features --features registered,x6x00
    exit 0
fi

echo "== $TARGET binary (offline: the crates fetched, Cargo.lock as it is)"
# No PulseAudio or ALSA (the radio's audio is the GUI's); the socket backend (x6x00) and registered
cargo build --frozen --release -p xara --bin xara --target $TARGET --no-default-features --features registered,x6x00
BIN=$SRC/target/$TARGET/release/xara

# The notices that go with the binary: XARA's licence, each crate compiled in (cargo tree, the crate's own MIT text:
# they are all MIT, some also Apache-2.0, Zlib or 0BSD, at the user's choice) and the Rust standard library's
DOC=$SRC/doc
mkdir "$DOC"
cp LICENSE "$DOC/LICENSE"
CRATES=$(cargo tree --frozen -e normal --target $TARGET -p xara --no-default-features --features registered,x6x00 \
             --prefix none | sed 's/ (\*)$//' | grep -v '^xara' | sort -u)
{
    echo "XARA $XARA_VER (NR5P's, commit ${XARA_COMMIT:0:12}), as built for OpenX6X00 (tools/xara), contains these"
    echo "crates, each used here under its MIT licence, and the Rust standard library of Rust $RUST_VER (Apache-2.0"
    echo "OR MIT; its notices in rust-std-COPYRIGHT.html.gz)."
    while read -r name ver; do
        d=$(ls -d "$CARGO_HOME"/registry/src/*/"$name-${ver#v}" 2> /dev/null | head -1 || true)
        [ -n "$d" ] || { echo "tools/xara/build.sh: no source of $name $ver in $CARGO_HOME" >&2; exit 1; }
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
