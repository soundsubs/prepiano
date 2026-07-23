# PrePiano

A physically-modelled, *preparable* piano for Ableton Move (Schwung), with a
VST3 build to follow. Struck-string waveguide synthesis you can mute with felt,
bow, re-hammer, and "prepare" — lay virtual knives, forks and screws across the
strings, the John Cage way.

Built to sit alongside [`soundsubs/noizboy`](https://github.com/soundsubs/noizboy)
and [`soundsubs/drmach`](https://github.com/soundsubs/drmach): one portable C
DSP core, a Schwung v2 plugin wrapper, and a desktop WAV renderer for
sound-design iteration.

```
prepiano/
├── src/
│   ├── prepiano_dsp.h / .c        # portable DSP core (no host deps)
│   ├── plugin/prepiano_plugin.c   # Schwung DSP Plugin API v2 wrapper
│   ├── host/plugin_api_v1.h       # reference stub of the Schwung ABI (see note)
│   └── modules/prepiano/
│       ├── module.json            # manifest: 8 chain-param knobs + dropdowns
│       └── ui.js                  # minimal Shadow UI screen
├── test/render_demo.c            # renders build/prepiano_demo.wav
├── scripts/
│   ├── build_desktop.sh          # compile + render the demo WAV
│   └── build_move.sh             # cross-compile ARM64, package, deploy
├── DESIGN.md                     # full knob-by-knob DSP design
└── Makefile
```

## Controls

| # | Knob | Range |
|---|------|-------|
| 1 | **Felt** | open → mostly muted (string damping) |
| 2 | **Attack** | plucked → bowed |
| 3 | **Decay** | ~0.15 s → many seconds |
| 4 | **Gauge** | normal → very heavy / stiff / hard to strike |
| 5 | **Hammer** | soft felt → metal |
| 6 | **Symp** | sympathetic resonance of nearby strings |
| 7 | **Disturb** | 0 → many prepared objects buzzing on the strings |
| 8 | **Reverb** | dry → 100% wet / far-away room |

Dropdowns (Voicing menu): **Polyphony** 16→1 (mono), **RNDMZ** (randomise all
knobs). See [DESIGN.md](DESIGN.md) for how each maps onto the string model.

## Desktop: build & audition

No dependencies beyond a C compiler and libm.

```bash
./scripts/build_desktop.sh          # -> build/prepiano_demo.wav
# or:
make demo && ./build/prepiano_demo out.wav
```

`test/render_demo.c` is also the quickest place to prototype patches — edit the
knob values and note sequence, rebuild, listen.

## Move: cross-compile & deploy

The Schwung DSP Plugin API v2 wrapper (`move_plugin_init_v2`) is in
`src/plugin/prepiano_plugin.c`. Build it into `dsp.so`, stage the module dir,
and copy to the device:

```bash
# against the real Schwung headers (recommended):
SCHWUNG_SRC=../schwung/src ./scripts/build_move.sh

# then deploy to a Move on your network:
SCHWUNG_SRC=../schwung/src MOVE_HOST=move.local DEPLOY=1 ./scripts/build_move.sh
```

Rescan from the Module Manager (`http://move.local:7700`) or re-enter Schwung
to load **PrePiano**. Requires the Schwung framework installed on the device —
see [charlesvestal/schwung](https://github.com/charlesvestal/schwung).

### About `src/host/plugin_api_v1.h`

That header is a **reference stub** of the Schwung host/plugin ABI (per
`docs/API.md`) so this repo compiles and type-checks on its own. On a real Move
build, point `SCHWUNG_SRC` at a Schwung `src/` checkout — its authoritative
header goes on the include path first and wins. If the real ABI differs from the
stub, the wrapper is the only file to reconcile; the DSP core is untouched.

## Status

v0.1 — DSP core complete and verified (accurate tuning, stable, no clipping),
Schwung wrapper + manifest in place, desktop renderer working. Roadmap and the
list of intentional v0.1 simplifications are at the end of
[DESIGN.md](DESIGN.md). VST3 wrapper is the next target and reuses
`prepiano_dsp.*` unchanged.

## Audio spec

44100 Hz, 128-frame blocks, stereo interleaved int16 — matches Move.
