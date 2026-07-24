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

**Knob 2 — Attack (plucked → gently bowed).** Two things move together. First, a
continuous stick-slip excitation is fed into the loop while the key is held, so
the note is *sustained by bowing* rather than decaying (self-limiting via
`× (1 − |string|)`, so it settles into a stable limit cycle instead of clipping).
Second, an **amplitude attack envelope** fades the note in over 0 → ~5 s, so at
high Attack the sound swells up gently the way a slow bow speaks, instead of
starting at full volume. At 0 the onset is instant (a pluck).

**Knob 3 — Decay.** Ring time, from ~0.08 s up to ~10 s of audible tail
(`g = 10^(−3·Δt/T60)`, with the raw T60 aimed a little high to offset the
in-loop damping). Barely-there staccato at the bottom, long sustain at the top.

**Knob 4 — Gauge.** String thickness/mass — *not* the number of strings (that's
automatic; see "Strings per note" below). Heavier gauge (a) adds *stiffness*,
which increases the dispersion allpass coefficient → stretched, inharmonic,
bell-like partials; (b) darkens the tone; (c) makes the string "harder to
strike" — the same key velocity delivers less energy. Tuning is compensated for
the extra allpass group delay so pitch stays put as you turn it.

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

## Strings per note (automatic, by register)

Real pianos don't put the same number of strings on every note: the lowest bass
notes have a single thick copper-wound string, the upper bass has two, and the
tenor and treble have three bare-steel strings per note (~230 strings over 88
keys). PrePiano models this automatically by register — monochord for roughly
the lowest octave (≤ E1), bichord through the upper bass (≤ D♯2), trichord from
around the bass/tenor break (E2) up. Each unison string is a full waveguide,
detuned from its neighbours by a couple of cents, so the course *beats* — that's
the piano's shimmer. The companion strings are also given a slightly longer ring
than the struck string, which reproduces the characteristic **double decay**: a
loud, fast-decaying attack followed by a quieter, long-sustaining aftersound as
the coupled strings trade energy. Output is level-normalised per string count,
and preparation rattle is applied to the course's main string.

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
* Tuning holds within **±0.6 cents A1–C7** (measured by DFT partial-tracking in
  `test/measure_partials.c`), down from ±8. The delay length subtracts the
  **exact phase delay each loop filter contributes _at the fundamental_** — the
  damping lowpass, the dispersion allpass cascade, and the DC blocker — instead
  of a DC group-delay approximation. Removing the stale `-1` structural offset
  and evaluating phase delay at f0 is what pinned the pitch across the register.

## Inharmonicity (v0.1.11 rework)

Real stiff-string stretch — `f_j = F0·√(1 + B·j²)` — comes from a **cascade of
first-order allpasses with a _negative_ coefficient** in the loop (2–3 sections).
Findings from `measure_partials.c`, which strikes a note and DFT-tracks partials
1–12:

* A single weak allpass does **nothing** audible: piano partials sit in the low-ω
  region where a first-order allpass is nearly flat, and the linear-interpolated
  fractional delay actively *compresses* partials. You need enough sections and
  strength to overcome the interpolator and net a *stretch*.
* Identical strong sections *ripple* (non-monotonic) once the partials climb into
  the steep part of the allpass phase curve. So the coefficient is scaled by
  register: strongest in the tenor (~0.80 at C3, a clean monotonic stretch),
  easing ~0.0139/semitone into the treble (fewer, gentler sections stay smooth),
  tapering into the deep bass (whose low partials read near-harmonic anyway).
* A uniform ~3.5-cent flat offset from the interpolated allpass is trimmed with a
  1.025 factor on the compensation term. Net: f0 in tune, partials stretch
  monotonically sharp across ~A1–C6.

## Unison strings (re-enabled v0.1.11)

Notes carry **1 / 2 / 3 detuned strings by register** (mono below ~E1, bichord to
~F♯2, trichord above), each a `440 / 441 / 439`-style **±3.9-cent** spread. They
beat → shimmer, and the **bridge coupling** in `voice_tick` drains the in-phase
common mode fast while the opposed modes ring on — the piano "double decay". Cost
is modest: reverb/sympathetic/body dominate, so 8 trichord voices still render
~142× realtime on x86 (≈4× on the Move); default polyphony stays at 6.

## What's deliberately simple (and where to go next)

* **Bass low-partials read slightly flat** (the linear interpolator wins at
  ultra-low ω). An **allpass fractional-delay interpolator** would remove that
  compression and let the bass stretch too.
* **Dispersion cascade is identical-section.** A purpose-designed high-order
  (Rauhala–Välimäki) allpass would match the target B curve more accurately,
  especially in the treble; more CPU per string.
* **Sympathetic bank is a fixed tuned set**, not true string-to-string coupling.
  A coupled-waveguide version would resonate the *actual* held notes.
* **Two-stage hammer** (felt↔metal as a spectral morph). A real nonlinear
  hammer-felt compression model would add velocity-dependent brightness.
* **No soundboard/body resonance** yet — a short convolution or a modal body
  filter on the bus would add air and thump.
* **Preparation is per-note randomised.** A future mode could pin objects to
  fixed strings so a preparation is stable across a performance.
