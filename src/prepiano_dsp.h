/*
 * prepiano_dsp.h  -  PrePiano: a prepared, physically-modelled piano
 *
 * Portable, dependency-free DSP core (no Schwung / host headers here).
 * The same core is used by:
 *   - the Schwung plugin wrapper (src/plugin/prepiano_plugin.c) on Move
 *   - the desktop test harness (test/render_demo.c) that renders a WAV
 *
 * Audio: internal processing in float; the host wrapper converts to
 * stereo interleaved int16 at 44100 Hz, 128-frame blocks.
 *
 * Coding style follows soundsubs/noizboy (noiseboy_dsp.c/h): a single
 * opaque state struct, an init, per-parameter setters keyed by an enum,
 * MIDI note on/off, and a block render.
 */
#ifndef PREPIANO_DSP_H
#define PREPIANO_DSP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PP_MAX_VOICES        8      /* Move CPU ceiling is low; keep this lean */
#define PP_MAX_PREP          6      /* max simultaneous "prepared" objects */
#define PP_SYMPATHETIC_TAPS  5      /* open-string resonator bank size     */

/* Parameter identifiers. These line up 1:1 with the eight physical knobs
 * on Move plus the two menu/dropdown parameters. All continuous params take
 * a normalised value in [0,1]; PP_P_POLYPHONY takes 1..16; PP_P_RNDMZ is an
 * action (any set() call triggers a randomise of the eight knobs). */
typedef enum {
    PP_P_FELT = 0,      /* knob 1: string damping, open -> mostly muted     */
    PP_P_ATTACK,        /* knob 2: excitation, plucked -> bowed             */
    PP_P_DECAY,         /* knob 3: ring time, short -> many seconds         */
    PP_P_GAUGE,         /* knob 4: string gauge/stiffness, normal -> heavy  */
    PP_P_HAMMER,        /* knob 5: striker, soft felt -> metal              */
    PP_P_SYMPATHETIC,   /* knob 6: sympathetic resonance of nearby strings  */
    PP_P_DISTURB,       /* knob 7: preparation, 0 -> many prepared sources  */
    PP_P_REVERB,        /* knob 8: stereo room reverb, dry -> 100% wet/far  */
    PP_P_POLYPHONY,     /* menu: voice count, 16 (poly) .. 1 (mono)         */
    PP_P_RNDMZ,         /* menu action: randomise all eight knobs           */
    PP_P_COUNT
} pp_param_t;

typedef struct pp_state pp_state_t;

/* Lifecycle */
pp_state_t *pp_create(float sample_rate);
void        pp_destroy(pp_state_t *st);
void        pp_reset(pp_state_t *st);           /* silence all voices       */

/* Parameters. value is normalised [0,1] for the eight knobs; for
 * PP_P_POLYPHONY pass the integer count as a float (1..16). */
void        pp_set_param(pp_state_t *st, pp_param_t id, float value);
float       pp_get_param(pp_state_t *st, pp_param_t id);

/* Notes. note = MIDI note 0..127, vel = 1..127. */
void        pp_note_on(pp_state_t *st, int note, int vel);
void        pp_note_off(pp_state_t *st, int note);
void        pp_all_notes_off(pp_state_t *st);

/* Randomise the eight continuous knobs (also invoked by PP_P_RNDMZ). The
 * RNG is internal and seedable so behaviour is reproducible off-hardware. */
void        pp_randomize(pp_state_t *st);
void        pp_seed(pp_state_t *st, uint32_t seed);

/* Render n frames of stereo float into out_l / out_r (may be the same
 * buffer stride if you deinterleave later). Values are ~[-1,1]. */
void        pp_render_float(pp_state_t *st, float *out_l, float *out_r, int n);

/* Convenience: render n frames straight to stereo interleaved int16
 * (L0,R0,L1,R1,...), as Move's render_block wants. */
void        pp_render_int16(pp_state_t *st, int16_t *out_lr, int n);

#ifdef __cplusplus
}
#endif
#endif /* PREPIANO_DSP_H */
