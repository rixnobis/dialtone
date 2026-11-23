# dialtone

![dialtone on a satellite station](docs/img/hero.png)

An incremental PS1 game about phoning in to radio contests. The DJ says "caller 5 wins $20", you
dial the station, and if you get through as the fifth caller the money is yours. Buy a redial
button, a second line and an autodialer that starts out dumb and learns to time its calls, climb
from a local station to a satellite one where prizes run to the millions, then move to a new area
code for Reputation and perks that make the next run faster. Numbers go past a billion and keep
going; progress saves to the memory card.

How to play, every upgrade and every number: [the manual](docs/manual.md).

Built on [psyqo-madnight-engine](https://github.com/Forceh91/psyqo-madnight-engine), pulled in
as a submodule and not modified.

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

`make DEFINES="PCDRV DIALTONE_START_CASH=300"` starts you with money, for trying the upgrades; it
counts as earned, so 2000000 opens every station and the first new area code. `tools/run.sh`
runs a headless session with `tools/autoplay.lua` driving the pad (`CARD=` keeps the memory card
between runs, `SEQ=` presses buttons at given frames). `tools/test_bignum.sh` checks the number
type on the host.

## Where things are

- `src/sim.cpp`: the game. Stations, contest rounds, busy/ringing odds, lines, upgrades, price
  curves, Reputation and perks. No rendering in here, so it is the file to change for balance.
- `src/bignum.cpp`: the number type, a 9-digit mantissa and a power of ten, integer only.
- `src/save.cpp`: the memory card save, loaded at boot and written in the background.
- `src/phone_scene.cpp`: the desk scene, input, the text overlay and the Reputation screen.
- `src/tones.cpp`: DTMF, ringback, busy and the win jingle, generated on the SPU from a
  two-block looping sine (loop start on the first block, end + repeat on the second), no sample
  files.
