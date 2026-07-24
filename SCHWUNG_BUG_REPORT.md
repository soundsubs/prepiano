# Custom sound_generator hangs the UI when highlighted in the module swap dropdown

**Module:** `prepiano` (custom-installed from `soundsubs/prepiano`, a physically-modelled piano)
**Host:** Schwung on Ableton Move
**api_version:** 2, `component_type: sound_generator`

## Symptom
Scrolling **to** PrePiano in the module swap dropdown hangs the Shadow UI — the
jog wheel stops scrolling and knobs stop responding for a few seconds, then
"catches up." Scrolling away recovers immediately; scrolling back to PrePiano
hangs again. The swap window never fully opens while PrePiano is the highlighted/
current module. Every other module — including my own custom-installed
`noiseboy` and `drmach` — behaves normally.

Once PrePiano is actually loaded, it **plays fine** (audio is correct). It's only
the dropdown highlight/preview/load interaction that hangs.

## What it is NOT (measured / ruled out)
- **Not registration.** After a reload, `module-snapshot.txt` contains
  `prepiano=0.1.10`.
- **Not the DSP's cost.** Measured off-device: `create_instance` ≈ **0.019 ms**;
  full render ≈ **160× realtime** at 8-voice polyphony. Cutting the DSP ~6–8×
  (16→8 voices, 3→1 strings/note, slimmer resonator banks) produced **no change**
  in the hang.
- **Not the manifest param surface.** Reproduces identically with `ui_hierarchy`,
  with `chain_params`, with both, and with `chain_params`-only (no
  `ui_hierarchy`).
- **Not logged.** Nothing in `schwung.log`, `schwung-manager.log`, or `debug.log`
  when it hangs (only SPI init + shim boot lines).
- Manager installs it cleanly (`custom module installed id=prepiano`).

## Module characteristics that differ from typical modules
- Larger per-instance state (~325 KB) than most synths, though `create_instance`
  is still sub-millisecond.
- `dsp.so` exports `move_plugin_init_v2`; standard v2 plugin API
  (`create_instance`/`destroy_instance`/`on_midi`/`set_param`/`get_param`/`render_block`).
- `get_param` returns -1 for keys it doesn't recognise (e.g. `ui_hierarchy`,
  `chain_params`, `name`) — could the dropdown-preview path be polling/retrying a
  `get_param` key and spinning when it gets -1?

## Question
What does the Shadow UI do when a `sound_generator` is **highlighted** in the swap
dropdown (instantiate? render a preview? query specific `get_param` keys?), and
what could cause that to block for a correctly-registered custom module whose
`create_instance`/`render` are both measurably fast? Happy to add logging or test
a build if that helps isolate it.
