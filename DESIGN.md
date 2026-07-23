# PrePiano — design & DSP reference

PrePiano is a physically-modelled, *preparable* piano. The clean tone is a
struck steel string; every knob bends that model toward something you could do
to a real piano — mute it with felt, bow it, swap the hammer for a coin, or lay
knives and forks across the strings à la John Cage.

The whole instrument is one dependency-free C core (`src/prepiano_dsp.*`) that
runs unchanged in three places: the desktop WAV renderer, the Schwung module on
Move, and (later) a VST3 wrapper. Everything below is what actually ships in
that core — no external libraries, no allocations after `pp_create()`.

## Why waveguides, not samples

A sampled piano can't be prepared: you can't ask a recording to buzz because a
bolt is resting on the string. PrePiano instead models each note as an extended
Karplus–Strong / digital-waveguide string — a delay line looped through a
damping filter and a dispersion allpass — which is the same lineage as
noizboy's Karplus–Strong layer. Because the *string itself* is simulated, the
preparations (buzzing objects, hammer changes, felt) are physical
modifications to that loop rather than effects bolted on afterward.

```
                    hammer / bow excitation
                             |
        +--------------------v--------------------------+
        |   fractional delay line  (pitch = SR / freq)  |
        |         |                          ^          |
        |   string damping LP  ->  dispersion allpass   |   <- one voice
        |   (Felt / Decay / Gauge)  (Gauge stiffness)   |
        |         |     prepared-object rattle (Disturb)|
        +---------|-------------------------------------+
                  |  voice out (equal-power keyboard pan)
   sum of 1..16 voices --> sympathetic resonator bank (Symp)
                        --> soft bus saturation
                        --> stereo Schroeder room reverb (Reverb) --> L / R
```

## The eight knobs

Each knob is a normalised `0..1` value that is mapped, at note-on, into the
per-voice coefficients of the string model.

**Knob 1 — Felt.** A practice-felt strip lowered onto the strings. Raises the
in-loop lowpass coefficient (darker, faster HF decay) *and* multiplies the loop
gain down so the note dies sooner. At the top of its range the string is nearly
dead — a soft, muted thud. `damp_coef = 0.15 + 0.75·felt + …`, `T60 ·= lerp(1,
0.12, felt)`.

**Knob 2 — Attack (plucked → bowed).** At 0 the string is struck once and left
to ring (impulsive pluck). As it turns up, a continuous stick-slip excitation is
fed into the loop while the key is held, so the note is *sustained by bowing*
rather than decaying. The bow term is self-limiting (`× (1 − |string|)`) so it
drives a stable limit cycle instead of integrating up to a clip.

**Knob 3 — Decay.** Ring time, from ~0.15 s to ~18 s, set through the loop gain
`g = 10^(−3·Δt/T60)`. Barely-there staccato at the bottom, cathedral sustain at
the top.

**Knob 4 — Gauge.** String thickness/mass. Heavier gauge (a) adds *stiffness*,
which increases the dispersion allpass coefficient → stretched, inharmonic,
bell-like partials; (b) darkens the tone; (c) makes the string "harder to
strike" — the same key velocity delivers less energy, so big strings need to be
leaned on. Tuning is compensated for the extra allpass group delay so pitch
stays put as you turn it.

**Knob 5 — Hammer (felt → metal).** Shapes the excitation burst. A soft felt
hammer is a longer, low-passed contact; a metal hammer is a short, bright click
with far more high-frequency content and a hard transient at the leading edge.
Contact time `lerp(6 ms, 0.8 ms, hammer)`.

**Knob 6 — Sympathetic.** A bank of nine lightly-damped "open string"
resonators tuned across a stack of octaves and fifths. All voices feed it and it
rings back underneath the played notes — the halo you hear with the sustain
pedal down. Scaled entirely by this knob (0 = off).

**Knob 7 — Disturb (the "prepared" knob).** From 0 to many prepared objects laid
on the strings. The knob sets how many objects exist system-wide; each struck
string randomly receives some of them, with randomised character (rattle
threshold, buzz frequency 600 Hz–5 kHz, dead-weight damping). When the string's
motion exceeds an object's threshold it *chatters* against it — a
velocity-gated metallic buzz that injects inharmonic content and saps a little
energy, exactly like a screw or a fork rattling on a vibrating string. Turn it
up and the clean piano is progressively interrupted and detuned.

**Knob 8 — Reverb.** Stereo Schroeder room (4 combs + 2 allpasses per channel).
As it goes from dry to 100% wet it also moves the source *away*: the dry signal
drops out, pre-delay grows, and a distance lowpass darkens the tail, so full
wet is a distant, diffuse room rather than just "more reverb".

## The two dropdowns

**Polyphony (16 → 1).** Caps the voice allocator. At 1 it's monophonic — one
string at a time, retriggered legato — which, with Disturb up, gives a gnarly
mono prepared-piano lead. Mid values (e.g. 4, 6) are useful for CPU and for a
more "one-piano-can-only-do-so-much" feel.

**RNDMZ.** Randomises all eight knobs into musically-biased territory (decay is
kept from being ultra-short, disturbance is skewed low so most rolls are
playable, reverb kept sane). Deterministic PRNG so a given state is
reproducible. Great for stumbling onto preparations you wouldn't dial by hand.

## Voice lifecycle & allocation

`pp_note_on` picks a slot within the polyphony limit: reuse a voice already on
that note, else a free slot, else steal the oldest. `voice_configure` snapshots
the current knobs into the voice and fills the delay line with the shaped hammer
burst. Held notes ring (and keep bowing) until `pp_note_off`; a released,
non-bowed voice is culled once it drops below the noise floor. Sustain pedal
(CC64) and all-notes-off (CC120/123) are handled in the Schwung wrapper.

## Numerical care

* Internal float; the host wrapper clips and converts to interleaved int16.
* Loop gain is clamped `< 1`, the bow is amplitude-limited, the sympathetic
  bank is lightly damped (fb 0.99), and the mix passes a `tanh` before reverb —
  so the model can't run away. Verified: 23 s tour renders finite, peak 0.76,
  0.00% clipping.
* Tuning holds within ±8 cents A1–C6 (measured by autocorrelation), with the
  loop-filter and dispersion group delays compensated in the delay length.

## What's deliberately simple in v0.1 (and where to go next)

* **Single dispersion allpass** per voice. A cascade of 2–4 would give more
  accurate high-register inharmonicity; easy to extend `voice_tick`.
* **Sympathetic bank is a fixed tuned set**, not true string-to-string coupling.
  A coupled-waveguide version would resonate the *actual* held notes.
* **Two-stage hammer** (felt↔metal as a spectral morph). A real nonlinear
  hammer-felt compression model would add velocity-dependent brightness.
* **No soundboard/body resonance** yet — a short convolution or a modal body
  filter on the bus would add air and thump.
* **Preparation is per-note randomised.** A future mode could pin objects to
  fixed strings so a preparation is stable across a performance.
