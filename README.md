# dialtone

A proof of concept for an incremental PS1 game about phoning in to radio contests. You sit at a
desk with a landline and a radio. The DJ says "caller 5 wins $20", you dial the station, and if
you get through as the fifth caller the money is yours. Spend it on a redial button, a second
line, and an autodialer that keeps both lines busy without you, or on three levelled stats:

- Quick fingers (5 levels, $20 to $260): faster dialing. Redial and the autodialer go from 6 to 1
  frames per digit, and the wait after the last digit drops from 30 to 5 frames.
- Luck (5 levels, $30 to $300): 6 points off the busy chance per level, never below 10%.
- Charm (5 levels, $25 to $280): prizes +15% per level, and $1 more for every call that gets on
  air without winning.

A bigger station tunes in once you have earned $200.

Built on [psyqo-madnight-engine](https://github.com/Forceh91/psyqo-madnight-engine), pulled in
as a submodule and not modified.

## Controls

A how-to-play card comes up at boot; X or START closes it. D-pad moves over the keypad, X dials the key, O hangs up, triangle redials, L1/R1 change station.
Select steps through the shop (the next desk item, then the three stats) and square buys what it
shows.

## Building

```
git clone --recursive https://github.com/rixnobis/dialtone
cd dialtone
make BUILD=ReleaseFast DEFINES=PCDRV
./tools/make_assets.sh
```

That gives `dialtone.ps-exe` plus `build/dialtone.bin`, the asset archive a PCDRV build reads off
the host. Run it with pcsx-redux:

```
cp dialtone.ps-exe build/
pcsx-redux -run -pcdrv -pcdrvbase build -loadexe build/dialtone.ps-exe
```

For a disc image instead, build without `DEFINES=PCDRV` (run `make clean` first, the engine is
built with the same defines) and run pcsx-redux's `authoring` tool on `toc.json`.

`make_assets.sh` needs Blender (headless is fine) and Python with numpy. The desk lives in
`tools/build_desk.py`, which also saves `blender/desk.blend` to look at and writes
`src/desk_layout.hh` with where the keys, handset and lamps go. The meshbins it produces are
committed, so you only need Blender to change the desk.

`make DEFINES="PCDRV DIALTONE_START_CASH=300"` starts you with money, for trying the upgrades.
`tools/run.sh` runs a headless session with `tools/autoplay.lua` driving the pad.

## Where things are

- `src/sim.cpp`: the game. Stations, contest rounds, busy/ringing odds, lines, upgrades. No
  rendering in here, so it is the file to change for balance.
- `src/phone_scene.cpp`: the desk scene, input and the text overlay.
- `src/tones.cpp`: DTMF, ringback, busy and the win jingle, generated on the SPU from a
  two-block looping sine (loop start on the first block, end + repeat on the second), no sample
  files.
