#!/usr/bin/env python3
# Packs the voice bank from a dialtone-assets checkout (https://github.com/rixnobis/dialtone-assets)
# into assets/VOICE/, where the archive picks it up (src/voice_lines.cpp reads it):
#
#   VINDEX.BIN   voices, (voice, kind) groups, lines, takes, resident lines; loaded at boot
#   RESn.BIN     the resident lines (up to 64 KB each), copied to sound RAM at boot
#   STRMnn.BIN   every take, each starting on a sector, read off the CD while the phone rings;
#                stored, not compressed (tools/make_toc.py), and at most STREAM_SECTORS each
#
#   python3 -I tools/pack_voices.py ../dialtone-assets [--map build/voice-pack-map.json]
#
# The manifest is manifest.json (version 2). Without one, lines.json + vag/<id>.vag (version 1) is
# read as one take per line. Without assets/VOICE the game runs with silent callers.
import json, os, struct, sys

KINDS = ["greet", "hangup", "nosale", "sale", "odd", "callback",
         "sup_welcome", "sup_met", "sup_missed", "sup_warn", "sup_ring", "sup_missedcall", "sup_promote", "sup_done",
         "note_first", "note_hint", "note_warn", "note_plea", "note_cut", "note_last"]
TIERS = ["residential", "small_business", "corporate"]
SECTOR = 2048
STREAM_SECTORS = 1000          # authoring stores a file of at most 1023 sectors
PART_BYTES = 0xA000            # VoiceLines::PART_BYTES, one take must fit
RESIDENT_BYTES = 0x7E000 - 0x2A000  # VoiceLines::RESIDENT_ADDR .. SPU_END
RES_FILE_MAX = 64 * 1024
CALLER_KINDS = ["greet", "hangup", "nosale", "sale"]  # a stock caller needs the first two
SUP_RESIDENT = ["sup_welcome", "sup_met", "sup_missed"]

args = sys.argv[1:]
map_path = None
if "--map" in args:
    i = args.index("--map")
    map_path = args[i + 1]
    del args[i:i + 2]
src = args[0]
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_manifest():
    path = os.path.join(src, "manifest.json")
    if os.path.exists(path):
        m = json.load(open(path))
        assert m.get("version") == 2, "manifest.json: want version 2"
        return m
    lines = json.load(open(os.path.join(src, "lines.json")))
    voices = sorted({l["voice"] for l in lines})
    return {"version": 2, "rate": 8000,
            "voices": [{"id": v, "tier": "corporate" if v == "sup" else "residential"} for v in voices],
            "lines": [dict(l, tier="corporate" if l["voice"] == "sup" else "residential",
                           takes=[f"vag/{l['id']}.vag"]) for l in lines]}


def read_vag(rel):
    data = open(os.path.join(src, rel), "rb").read()
    assert data[:4] == b"VAGp", rel
    size, rate = struct.unpack(">II", data[12:20])
    body = data[48:48 + size]
    assert len(body) == size and size % 16 == 0, rel
    return body, rate


m = load_manifest()
retired = {v["id"] for v in m["voices"] if v.get("retired")}  # kept in the assets for their history only
m["voices"] = [v for v in m["voices"] if v["id"] not in retired]
m["lines"] = [l for l in m["lines"] if l["voice"] not in retired]
voices = [v["id"] for v in m["voices"]]
tier_of = {v["id"]: TIERS.index(v["tier"]) for v in m["voices"]}
assert len(voices) <= 255

# lines sorted by (voice, kind) so each group is one run of the line table
lines = []
dropped = 0
for l in m["lines"]:
    if l["kind"] not in KINDS or l["voice"] not in tier_of:
        print(f"skipping {l['id']}: kind {l['kind']} / voice {l['voice']} not packed", file=sys.stderr)
        continue
    takes = []
    for t in l["takes"]:
        body, rate = read_vag(t)
        if rate != m["rate"]:
            sys.exit(f"{t}: {rate} Hz, the bank is {m['rate']} Hz")
        if len(body) > PART_BYTES:
            print(f"dropping take {t}: {len(body)} bytes, a stream slot holds {PART_BYTES}", file=sys.stderr)
            dropped += 1
            continue
        takes.append((t, body))
    if takes:
        lines.append((voices.index(l["voice"]), KINDS.index(l["kind"]), l["id"], takes))
lines.sort(key=lambda x: (x[0], x[1], x[2]))

out = os.path.join(root, "assets", "VOICE")
os.makedirs(out, exist_ok=True)
for f in os.listdir(out):
    os.remove(os.path.join(out, f))

# every take into the stream files, each on its own sectors
streams, stream, take_rows, take_map = [], b"", [], []
line_rows, groups = [], {}
for li, (v, k, lid, takes) in enumerate(lines):
    groups.setdefault((v, k), [li, 0])[1] += 1
    line_rows.append(struct.pack("<HBB", len(take_rows), len(takes), 0))
    for t, body in takes:
        padded = body + b"\0" * (-len(body) % SECTOR)
        if len(stream) + len(padded) > STREAM_SECTORS * SECTOR:
            streams.append(stream)
            stream = b""
        take_rows.append(struct.pack("<BBHH", len(streams), 0, len(stream) // SECTOR, len(body) // 16))
        take_map.append({"line": lid, "take": t, "file": len(streams), "sector": len(stream) // SECTOR,
                         "bytes": len(body)})
        stream += padded
streams.append(stream)
assert len(take_rows) < 65536 and len(streams) < 100

# resident lines: the supervisor's shortest of each kind the game plays, then stock callers (the
# shortest greet / hangup / no-sale / sale of a voice), alternating tiers, while sound RAM lasts
def shortest(v, k):
    best = None
    for (lv, lk, lid, takes) in lines:
        if lv == v and lk == k:
            for t, body in takes:
                if best is None or len(body) < len(best[1]):
                    best = (t, body)
    return best


resident, used = [], 0
for k in SUP_RESIDENT:
    cands = [(len(b), t, b, v) for v in range(len(voices)) if (s := shortest(v, KINDS.index(k))) for t, b in [s]]
    if cands:
        _, t, b, v = min(cands)
        resident.append((v, KINDS.index(k), t, b))
        used += len(b)
stock = {0: [], 1: []}
for v, vid in enumerate(voices):
    if tier_of[vid] in stock:
        picks = [shortest(v, KINDS.index(k)) for k in CALLER_KINDS]
        if picks[0] and picks[1]:
            stock[tier_of[vid]].append((sum(len(p[1]) for p in picks if p), v, picks))
for t in stock:
    stock[t].sort(key=lambda x: (x[0], x[1]))
order = []
while stock[0] or stock[1]:
    for t in (0, 1):
        if stock[t]:
            order.append(stock[t].pop(0))
for size, v, picks in order:
    if used + size > RESIDENT_BYTES:
        continue
    for k, p in zip(CALLER_KINDS, picks):
        if p:
            resident.append((v, KINDS.index(k), p[0], p[1]))
    used += size

res_rows, res_files, res_map, res, offset = [], [], [], b"", 0
for v, k, t, b in resident:
    if len(res) + len(b) > RES_FILE_MAX:
        res_files.append(res)
        res = b""
    res_rows.append(struct.pack("<BBHI", v, k, len(b) // 16, offset))
    res_map.append({"voice": voices[v], "kind": KINDS[k], "take": t, "offset": offset, "bytes": len(b)})
    res += b
    offset += len(b)
res_files.append(res)

index = b"DTV2" + struct.pack("<HHHBBHH", len(voices), len(line_rows), len(take_rows), len(streams), len(KINDS),
                              len(res_rows), m["rate"])
index += b"".join(struct.pack("<BBH", tier_of[v], 0, 0) for v in voices)
for v in range(len(voices)):
    for k in range(len(KINDS)):
        first, count = groups.get((v, k), (0, 0))
        index += struct.pack("<HH", first, count)
index += b"".join(line_rows) + b"".join(take_rows) + b"".join(res_rows)
index += struct.pack("<B", len(res_files)) + b"".join(struct.pack("<I", len(f)) for f in res_files)
open(os.path.join(out, "VINDEX.BIN"), "wb").write(index)
for i, f in enumerate(res_files):
    open(os.path.join(out, f"RES{i}.BIN"), "wb").write(f)
for i, f in enumerate(streams):
    open(os.path.join(out, f"STRM{i:02d}.BIN"), "wb").write(f)
if map_path:
    os.makedirs(os.path.dirname(os.path.abspath(map_path)), exist_ok=True)
    json.dump({"voices": voices, "kinds": KINDS, "lines": [l[2] for l in lines], "takes": take_map,
               "resident": res_map}, open(map_path, "w"), indent=1)
total = sum(len(s) for s in streams)
print(f"{len(line_rows)} lines, {len(take_rows)} takes ({dropped} dropped), {len(voices)} voices; "
      f"{len(streams)} stream files, {total} bytes; {len(res_rows)} resident lines, {used} of {RESIDENT_BYTES} "
      f"bytes; index {len(index)} bytes")
