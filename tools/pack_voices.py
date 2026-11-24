#!/usr/bin/env python3
# Packs the caller voices from a dialtone-assets checkout (https://github.com/rixnobis/dialtone-assets)
# into assets/VOICE/, where the archive picks them up: INDEX.BIN (one entry per line) and BANKn.BIN
# (the ADPCM bodies, up to 64 KB each so a bank fits in main RAM while it is copied to sound RAM).
#
#   python3 -I tools/pack_voices.py ../dialtone-assets
#
# Without assets/VOICE the game runs as it always has, callers silent (src/voice_lines.cpp).
import json, os, struct, sys

KINDS = ["greet", "hangup", "nosale", "sale", "sup_welcome", "sup_met", "sup_missed"]
BANK_MAX = 64 * 1024
SPU_SPACE = 0x7e000 - 0x2000  # VoiceLines::SPU_ADDR .. SPU_END

src = sys.argv[1]
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
lines = json.load(open(os.path.join(src, "lines.json")))
voices = sorted({l["voice"] for l in lines})
out = os.path.join(root, "assets", "VOICE")
os.makedirs(out, exist_ok=True)
for f in os.listdir(out):
    os.remove(os.path.join(out, f))

entries, banks, bank, offset = [], [], b"", 0
for l in lines:
    data = open(os.path.join(src, "vag", l["id"] + ".vag"), "rb").read()
    assert data[:4] == b"VAGp", l["id"]
    size, rate = struct.unpack(">II", data[12:20])
    body = data[48:48 + size]
    assert len(body) == size and size % 16 == 0, l["id"]
    if offset + size > SPU_SPACE:
        sys.exit(f"{l['id']}: the voices no longer fit in sound RAM ({offset + size} > {SPU_SPACE})")
    if len(bank) + size > BANK_MAX:
        banks.append(bank)
        bank = b""
    entries.append(struct.pack("<BBHII", KINDS.index(l["kind"]), voices.index(l["voice"]), rate, offset, size))
    bank += body
    offset += size
banks.append(bank)
index = b"DTVX" + struct.pack("<HH", len(entries), len(banks)) + b"".join(entries)
index += b"".join(struct.pack("<I", len(b)) for b in banks)
open(os.path.join(out, "INDEX.BIN"), "wb").write(index)
for i, b in enumerate(banks):
    open(os.path.join(out, f"BANK{i}.BIN"), "wb").write(b)
print(f"{len(entries)} lines, {len(voices)} voices, {len(banks)} banks, {offset} bytes of {SPU_SPACE} sound RAM")
