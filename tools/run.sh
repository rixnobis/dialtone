#!/bin/sh
# run.sh OUTDIR "shot,vsyncs" quit_at : headless pcsx-redux run of the PCDRV build with the
# autoplay script, screenshots converted to png in OUTDIR
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
EMU=${EMU:-pcsx-redux}
OUT=$1
rm -rf "$OUT"
mkdir -p "$OUT"
cp "$ROOT/dialtone.ps-exe" "$ROOT/build/"
cd "$ROOT/build"
SHOTDIR=$OUT SHOTS=$2 QUIT_AT=$3 timeout $(($3 / 50 + 60)) xvfb-run -a -s "-screen 0 1280x960x24" "$EMU" \
	-no-ui -testmode -run -stdout -lua_stdout -pcdrv -pcdrvbase "$ROOT/build" \
	-dofile "$ROOT/tools/autoplay.lua" -loadexe dialtone.ps-exe > "$OUT/log" 2>&1
for f in "$OUT"/*.raw; do
	python3 -I "$ROOT/tools/raw2png.py" "$f" 320 240 "${f%.raw}.png"
done
grep -E 'DIALTONE|SHOT' "$OUT/log"
