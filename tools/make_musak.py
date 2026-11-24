#!/usr/bin/env python3
# Generates src/musak_data.inc: the office musak for phone-books mode. Five instruments are synthesised
# here and encoded to SPU ADPCM (a few hundred bytes each), and a 16-bar bossa loop is written out as
# a note table for the sequencer in src/musak.cpp.
#
#   python3 -I tools/make_musak.py > src/musak_data.inc
#
# Pitched instruments are cycle-locked: every 16-byte ADPCM block holds exactly one period (28
# samples), so a note's pitch register is f * 28 * 4096 / 44100 like src/tones.cpp. The first blocks
# carry the attack (extra harmonics fading out), the last two loop forever (flag 0x04 on the first,
# 0x03 on the second) and the SPU's ADSR shapes the note. Shaker and rim are short one-shots.
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spu_adpcm  # noqa: E402

N = 28
AMP = 20000


def cycles(fn, attack_blocks):
    """fn(x, k) -> sample for phase x in one period of block k; k >= attack_blocks is the loop."""
    x = 2 * math.pi * np.arange(N) / N
    blocks = [fn(x, k) for k in range(attack_blocks + 2)]
    s = np.concatenate(blocks)
    s = s / np.max(np.abs(s)) * AMP
    flags = [0] * attack_blocks + [0x04, 0x03]
    return spu_adpcm.encode(np.round(s).astype(int), flags)


def ep(x, k):
    tine = math.exp(-k / 3.0) if k < 10 else 0.0
    return np.sin(x) + 0.22 * np.sin(2 * x) + 0.07 * np.sin(3 * x) + tine * (0.5 * np.sin(4 * x) + 0.25 * np.sin(7 * x))


def vibe(x, k):
    strike = math.exp(-k / 2.0) if k < 8 else 0.0
    return np.sin(x) + 0.04 * np.sin(4 * x) + strike * (0.45 * np.sin(4 * x) + 0.2 * np.sin(10 * x))


def bass(x, k):
    pluck = math.exp(-k / 2.5) if k < 8 else 0.0
    return (np.sin(x) + 0.35 * np.sin(2 * x) + 0.12 * np.sin(3 * x) +
            pluck * (0.35 * np.sin(3 * x) + 0.25 * np.sin(4 * x) + 0.15 * np.sin(5 * x)))


def oneshot(sig):
    s = sig / np.max(np.abs(sig)) * AMP
    return spu_adpcm.encode(np.round(s).astype(int))


rng = np.random.RandomState(2433)
t = np.arange(int(22050 * 0.07)) / 22050.0
noise = rng.uniform(-1, 1, len(t))
noise = np.diff(noise, prepend=0)  # a first difference: tilts the noise up, a brighter shaker
shaker = noise * np.minimum(1, t / 0.004) * np.exp(-t / 0.018)
t2 = np.arange(int(22050 * 0.05)) / 22050.0
rim = (np.sin(2 * math.pi * 1750 * t2) * np.exp(-t2 / 0.008) + 0.6 * rng.uniform(-1, 1, len(t2)) * np.exp(-t2 / 0.003))

# ADSR as the 32-bit value psyqo's ChannelPlaybackConfig takes: ADSR2 << 16 | ADSR1.
# ADSR1: attack mode/shift/step, decay shift, sustain level. ADSR2: sustain mode/dir/shift/step, release.
def adsr(a_exp, a_shift, a_step, d_shift, s_level, s_exp, s_dec, s_shift, s_step, r_exp, r_shift):
    a1 = (a_exp << 15) | (a_shift << 10) | (a_step << 8) | (d_shift << 4) | s_level
    a2 = (s_exp << 15) | (s_dec << 14) | (s_shift << 8) | (s_step << 6) | (r_exp << 5) | r_shift
    return (a2 << 16) | a1


instruments = [
    # name, data, adsr, sample rate (one-shots) or 0 (cycle-locked)
    ("EP", cycles(ep, 12), adsr(0, 6, 3, 12, 6, 1, 1, 16, 0, 1, 12), 0),
    ("VIBE", cycles(vibe, 8), adsr(0, 6, 3, 13, 8, 1, 1, 15, 0, 1, 14), 0),
    ("BASS", cycles(bass, 8), adsr(0, 4, 3, 11, 9, 1, 1, 17, 0, 1, 10), 0),
    ("SHAKER", oneshot(shaker), adsr(0, 0, 0, 0, 15, 0, 0, 31, 0, 0, 0), 22050),
    ("RIM", oneshot(rim), adsr(0, 0, 0, 0, 15, 0, 0, 31, 0, 0, 0), 22050),
]

# ---- the tune: 100 bpm, 16th notes, 16 bars ----
NAMES = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def midi(n):
    """'Eb4' -> 63"""
    base = NAMES[n[0]]
    i = 1
    while n[i] in "#b":
        base += 1 if n[i] == "#" else -1
        i += 1
    return base + 12 * (int(n[i:]) + 1)


chords = [  # (root for the bass, voicing), one per bar; bar 16 splits into two halves
    ("C2", "E4 G4 B4 D5"), ("A1", "C4 E4 G4 B4"), ("D2", "C4 E4 F4 A4"), ("G1", "B3 E4 F4 A4"),
    ("E2", "D4 E4 G4 B4"), ("A1", "C#4 E4 G4 Bb4"), ("D2", "C4 E4 F4 A4"), ("G1", "B3 E4 F4 A4"),
    ("F2", "E4 G4 A4 C5"), ("F2", "D4 F4 Ab4 C5"), ("E2", "D4 E4 G4 B4"), ("A1", "C#4 E4 G4 A4"),
    ("D2", "C4 E4 F4 A4"), ("G1", "B3 E4 F4 A4"), ("C2", "B3 D4 E4 G4"), None,
]
turnaround = [("D2", "C4 D4 F4 A4"), ("G1", "B3 D4 F4 G4")]

melody = [  # per bar: (16th, note, length)
    [(2, "G5", 4), (6, "E5", 4), (10, "D5", 6)],
    [(0, "C5", 6), (8, "E5", 6)],
    [(2, "F5", 4), (6, "E5", 2), (8, "D5", 4), (12, "C5", 4)],
    [(0, "B4", 10), (12, "A4", 2), (14, "B4", 2)],
    [(0, "D5", 6), (6, "B4", 2), (8, "G5", 8)],
    [(0, "E5", 4), (4, "C#5", 4), (8, "Bb4", 8)],
    [(0, "A4", 6), (6, "C5", 2), (8, "E5", 8)],
    [(0, "F5", 4), (4, "E5", 4), (8, "D5", 4)],
    [(2, "A5", 6), (8, "G5", 4), (12, "E5", 4)],
    [(0, "Ab5", 6), (6, "F5", 2), (8, "D5", 8)],
    [(0, "G5", 6), (6, "E5", 2), (8, "B4", 8)],
    [(0, "C#5", 4), (4, "E5", 4), (8, "G5", 6)],
    [(0, "F5", 10), (10, "E5", 2), (12, "D5", 4)],
    [(0, "E5", 6), (6, "D5", 2), (8, "B4", 6)],
    [(0, "C5", 12)],
    [(8, "D5", 4), (12, "B4", 4)],
]

# tracks: 0 bass, 1-4 the e-piano chord, 5 vibe melody, 6 shaker, 7 rim
events = []


def add(at, track, note, length, vel):
    events.append((at, track, note, length, vel))


comp = [[(0, 3), (6, 2), (10, 4)], [(2, 3), (6, 2), (12, 3)]]  # two-bar comping figure
clave = [[0, 6, 12], [4, 10]]
for bar in range(16):
    at = bar * 16
    parts = [(chords[bar], 0, 16)] if chords[bar] else [(turnaround[0], 0, 8), (turnaround[1], 8, 8)]
    for (root, voicing), off, span in parts:
        r = midi(root)
        fifth = r + 7
        hits = [(0, r, 5), (6, fifth, 2), (8, fifth, 5), (14, r, 2)] if span == 16 else [(0, r, 5), (6, fifth, 2)]
        for pos, n, ln in hits:
            add(at + off + pos, 0, n, ln, 112 if pos in (0, 8) else 84)
        notes = [midi(n) for n in voicing.split()]
        figure = comp[bar % 2] if span == 16 else [(0, 3), (4, 2)]
        for pos, ln in figure:
            if pos < span:
                for i, n in enumerate(notes):
                    add(at + off + pos, 1 + i, n, ln, 78 if pos == 0 else 66)
    for pos, n, ln in melody[bar]:
        add(at + pos, 5, midi(n), ln, 100)
    for pos in range(0, 16, 2):
        add(at + pos, 6, 0, 1, 60 if pos % 4 == 2 else 34)
    for pos in clave[bar % 2]:
        add(at + pos, 7, 0, 1, 44)

events.sort(key=lambda e: (e[0], e[1]))


def pitch_for(note):
    f = 440.0 * 2 ** ((note - 69) / 12)
    return round(f * N * 4096 / 44100)


print("// generated by tools/make_musak.py, do not edit")
print("// clang-format off")
offset = 0
for name, data, env, rate in instruments:
    print(f"static constexpr uint8_t c_inst{name}[{len(data)}] __attribute__((aligned(4))) = {{")
    for i in range(0, len(data), 16):
        print("\t" + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
    print("};")
print("static const MusakInstrument c_instruments[] = {")
for name, data, env, rate in instruments:
    print(f"\t{{c_inst{name}, {len(data)}, 0x{env:08x}u, {rate}}},")
    offset += len(data)
print("};")
print(f"static constexpr uint32_t MUSAK_SAMPLE_BYTES = {offset};")
print("static constexpr uint16_t MUSAK_LOOP_STEPS = 256;")
print("static const MusakEvent c_events[] = {")
for at, track, note, length, vel in events:
    p = pitch_for(note) if track <= 5 else 0
    print(f"\t{{{at}, {track}, {length}, {vel}, {p}}},")
print("};")
print(f"// {len(events)} events")
