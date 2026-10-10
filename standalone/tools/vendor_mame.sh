#!/bin/sh
# Copy the exact MAME source files the standalone compiles (tools/mame_files.txt) plus Berkeley SoftFloat 3e into
# third_party/mame, preserving MAME's paths, and record provenance (commit, local modifications, SHA256 manifest).
# Usage: tools/vendor_mame.sh [MAME_TREE]   (default ~/EIII_RE/mame).  Re-run after changing any of those MAME files.
set -e
MAME=${1:-$HOME/EIII_RE/mame}
HERE=$(cd "$(dirname "$0")/.." && pwd)
DST=$HERE/third_party/mame
rm -rf "$DST"; mkdir -p "$DST"
while read -r f; do mkdir -p "$DST/$(dirname "$f")"; cp "$MAME/$f" "$DST/$f"; done < "$HERE/tools/mame_files.txt"
mkdir -p "$DST/3rdparty"
cp -r "$MAME/3rdparty/softfloat3" "$DST/3rdparty/softfloat3"
rm -rf "$DST/3rdparty/softfloat3/doc"
# SoftFloat file list exactly as MAME's scripts/src/3rdparty.lua builds it
awk '/^project "softfloat3"/{p=1} p && /3rdparty\/softfloat3\/(source|bochs_ext)\/.*\.c"/{gsub(/.*MAME_DIR \.\. "/,""); gsub(/",?$/,""); print} p && /^project "/ && !/softfloat3/{p=0}' \
	"$MAME/scripts/src/3rdparty.lua" > "$DST/softfloat3_files.txt"
REV=$(git -C "$MAME" rev-parse HEAD)
git -C "$MAME" diff HEAD -- $(cat "$HERE/tools/mame_files.txt") > "$DST/LOCAL_CHANGES.diff" || true
( cd "$DST" && find . -type f ! -name MANIFEST.sha256 | sort | xargs sha256sum > MANIFEST.sha256 )
cat > "$DST/PROVENANCE.md" <<EOT
# Vendored MAME sources

Copied by \`tools/vendor_mame.sh\` from \`$MAME\` at commit \`$REV\`, **including the working-tree modifications**
listed in \`LOCAL_CHANGES.diff\` (the proven EIII SCSI behaviour: \`ncr5380.cpp\`, \`nscsi_hle.cpp/.h\`, \`bus/nscsi/cd.cpp\`).
Files are unmodified copies of that working tree; \`MANIFEST.sha256\` pins every byte.

Licences: every MAME file here is \`license:BSD-3-Clause\` (src/lib/util/disasmintf.cpp carries no tag; MAME's src/lib is
BSD-3-Clause). Berkeley SoftFloat 3e: see \`3rdparty/softfloat3/COPYING.txt\` (BSD-style, John R. Hauser).
No E-mu ROM, operating system, sample or bank data is included anywhere in this tree.
EOT
echo "vendored $(wc -l < "$HERE/tools/mame_files.txt") MAME files + SoftFloat 3e ($(wc -l < "$DST/softfloat3_files.txt") sources) from $REV"
