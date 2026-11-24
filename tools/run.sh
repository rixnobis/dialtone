#!/bin/sh
# run.sh OUTDIR "shot,vsyncs" quit_at : headless pcsx-redux run of the PCDRV build with the
# autoplay script, screenshots converted to png in OUTDIR. CARD=path keeps the memory card in slot 1
# somewhere that survives between runs (default: a fresh card inside OUTDIR)
# TIMEOUT=seconds overrides the wall-clock limit, for long runs on a busy box
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
EMU=${EMU:-pcsx-redux}
OUT=$1
rm -rf "$OUT"
mkdir -p "$OUT"
CARD=${CARD:-$OUT/card1.mcd}
cp "$ROOT/dialtone.ps-exe" "$ROOT/build/"
cd "$ROOT/build"
SHOTDIR=$OUT SHOTS=$2 QUIT_AT=$3 timeout ${TIMEOUT:-$(($3 / 50 + 60))} xvfb-run -a -s "-screen 0 1280x960x24" "$EMU" \
	-no-ui -testmode -run -stdout -lua_stdout -pcdrv -pcdrvbase "$ROOT/build" \
	-memcard1 "$CARD" -dofile "$ROOT/tools/autoplay.lua" -loadexe dialtone.ps-exe > "$OUT/log" 2>&1
for f in "$OUT"/*.raw; do
	# shots taken before the game sets its video mode are another size
	python3 -I "$ROOT/tools/raw2png.py" "$f" 320 240 "${f%.raw}.png" 2>/dev/null || echo "skipped $f"
done
grep -E 'DIALTONE|SHOT' "$OUT/log"
