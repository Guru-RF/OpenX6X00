#!/bin/sh
# docs/MANUAL.md as a PDF, to attach to a release (issue #138):
#   sh tools/manual_pdf/build.sh [out.pdf] [ref]
# out.pdf: OpenX6X00-manual-v<version>.pdf in the current directory by default, the version from src/main.h.
# ref: the git ref the PDF's links into the repository go to on GitHub (README, THEMES.md...), v<version> by
# default: the release's tag. The manual's own links (the contents, "see ...") stay inside the PDF, and
# WeasyPrint makes the bookmarks from the headings. Needs pandoc and weasyprint (and the Noto Sans fonts).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
VER=$(sed -n 's/.*define VERSION ".* v\([0-9.]*\)".*/\1/p' "$TOP/src/main.h")
[ -n "$VER" ] || { echo "no version in src/main.h" >&2; exit 1; }
OUT=${1:-OpenX6X00-manual-v$VER.pdf}
REF=${2:-v$VER}
for t in pandoc weasyprint; do
    command -v $t > /dev/null || { echo "$t is needed" >&2; exit 1; }
done

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
DATE=$(git -C "$TOP" log -1 --format=%cs 2>/dev/null || date -u +%F)
cat > "$TMP/title.html" << EOT
<section class="title-page">
<h1>OpenX6X00</h1>
<p>Manual</p>
<p class="version">v$VER</p>
<p>$DATE</p>
<p>github.com/Guru-RF/OpenX6X00</p>
</section>
EOT
# gfm: GitHub's heading ids, so the manual's #links land where they do on GitHub
MANUAL_REF=$REF MANUAL_DOCS="$TOP/docs" pandoc "$TOP/docs/MANUAL.md" -f gfm -t html5 --standalone \
    --metadata title="OpenX6X00 v$VER manual" --metadata pagetitle="OpenX6X00 v$VER manual" \
    --include-before-body "$TMP/title.html" \
    --lua-filter "$HERE/links.lua" --css "$HERE/manual.css" \
    -V title= -o "$TMP/manual.html"
# The screenshots are found from docs/, as on GitHub
weasyprint --base-url "$TOP/docs/" -s "$HERE/manual.css" "$TMP/manual.html" "$OUT"
echo "$OUT: $(du -h "$OUT" | cut -f1)"
