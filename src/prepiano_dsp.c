/*
 * prepiano_dsp.c  -  PrePiano DSP core
 *
 * A physically-modelled piano built on extended Karplus-Strong / digital
 * waveguide strings, with a "preparation" layer (buzzing objects laid on the
 * strings) and a sympathetic-resonance bank. See DESIGN.md for the full map
 * of each knob to its physical meaning.
 *
 * Signal flow per note:
 *
 *   hammer excitation ---> [ string waveguide loop ]---> voice out
 *        (felt<->metal)         |  ^  damping (felt/decay)
 *                               |  |  dispersion (gauge/stiffness)
 *                               |  +--preparation rattle (disturb)
 *                               v
 *                          sympathetic bus --> resonator bank --+
 *                                                               v
 *   sum of voices ---------------------------------------> [ room reverb ] --> L/R
 *
 * Everything is float internally. No allocations after pp_create().
 */
#include "prepiano_dsp.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Longest delay line: lowest usable note ~ MIDI 21 (A0, 27.5 Hz).
 * 44100 / 27.5 ~= 1604 samples; round up with headroom. */
#define PP_DELAY_MAX 2400

/* soundboard body resonator count */
#define PP_BODY_MODES 10

/* ----------------------------------------------------------------------- *
 *  Small helpers
 * ----------------------------------------------------------------------- */
static inline float pp_clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
static inline float pp_lerp(float a, float b, float t) {
    return a + (b - a) * t;
}
/* soft saturator, gentle odd-harmonic character */
static inline float pp_tanhf(float x) {
    /* cheap tanh approximation, monotonic and bounded */
    if (x < -3.0f) return -1.0f;
    if (x >  3.0f) return  1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Deterministic PRNG (xorshift32). No time / OS entropy so behaviour is
 * reproducible on hardware and in tests. */
static inline uint32_t pp_xs(uint32_t *s) {
    uint32_t x = *s ? *s : 0x1234567u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}
static inline float pp_frand(uint32_t *s) {            /* [0,1)  */
    return (pp_xs(s) >> 8) * (1.0f / 16777216.0f);
}
static inline float pp_frand_bi(uint32_t *s) {         /* [-1,1) */
    return pp_frand(s) * 2.0f - 1.0f;
}

/* ----------------------------------------------------------------------- *
 *  Per-string preparation ("prepared object" sitting on the string)
 * ----------------------------------------------------------------------- */
typedef struct {
    int   active;
    float thresh;      /* amplitude above which the object rattles/buzzes   */
    float buzz_amt;    /* how much metallic buzz it injects                 */
    float rate;        /* rattle oscillator rate (radians/sample)          */
    float phase;
    float mute;        /* extra damping the dead weight adds                */
    float lp;          /* state for the buzz colouring filter              */
} pp_prep_t;

/* ----------------------------------------------------------------------- *
 *  A single string (one delay line). Real piano notes have 1..3 unison
 *  strings per note; each is a slightly-detuned copy of this.
 * ----------------------------------------------------------------------- */
#define PP_MAX_STRINGS 3
#define PP_DISP_STAGES 1   /* allpass -> subtle stiff-string colour (see DESIGN) */
#define PP_COUPLING    0.005f /* unison-string bridge coupling (double decay)   */
typedef struct {
    float buf[PP_DELAY_MAX];
    int   widx;
    float delay;           /* fractional delay (detuned per string)        */
    float gain;            /* per-string loop gain (aftersound spread)     */
    float damp_z;          /* one-pole lowpass (string damping) state      */
    float ap_z[PP_DISP_STAGES], ap_x[PP_DISP_STAGES];  /* dispersion cascade */
    float dc_x, dc_y;      /* DC blocker in the feedback path              */
} pp_string_t;

/* ----------------------------------------------------------------------- *
 *  Voice (one struck note: a course of 1..3 unison strings)
 * ----------------------------------------------------------------------- */
typedef struct {
    int   active;
    int   note;
    int   age;             /* for voice stealing (older = higher)          */
    int   quiet;           /* consecutive near-silent samples (for culling)*/
    float vel;             /* 0..1                                          */
    float freq;

    pp_string_t str[PP_MAX_STRINGS];
    int   nstr;            /* 1 (bass) .. 3 (treble) unison strings        */
    float out_scale;       /* level normalisation for the string count     */

    /* shared string coefficients (same across the course) */
    float loop_gain;       /* base < 1, sets decay (string 0)             */
    float damp_coef;       /* 0..~0.95, string brightness/felt            */
    float disp_coef;       /* allpass coefficient, string stiffness       */
    float pan_l, pan_r;    /* equal-power keyboard pan gains              */

    /* amplitude attack envelope (gentle-bow swell, 0..~5 s) */
    float amp_env;         /* current gain, ramps 0 -> 1                  */
    float amp_inc;         /* per-sample increment                        */

    /* excitation */
    int   exc_len;         /* hammer burst length in samples               */
    float exc_gain;
    float bow_amt;         /* 0 = pluck, 1 = sustained bow                 */
    float bow_lp;          /* bow noise colouring                          */
    int   held;            /* note is still down (bow keeps exciting)      */

    /* preparation objects (applied to the main string of the course) */
    pp_prep_t prep[PP_MAX_PREP];

    uint32_t rng;
} pp_voice_t;

/* How many unison strings a note has, by register (realistic piano scaling):
 *   monochord (1)  : lowest bass, ~A0..E1
 *   bichord   (2)  : upper bass,  ~F1..D#2
 *   trichord  (3)  : tenor+treble, ~E2 and up
 * Exact break notes vary by instrument; these are typical. */
static int strings_for_note(int note) {
    if (note <= 28) return 1;   /* .. E1  */
    if (note <= 39) return 2;   /* .. D#2 */
    return 3;                   /* E2 ..  */
}

/* ----------------------------------------------------------------------- *
 *  Sympathetic resonator bank (a set of undamped-ish open strings)
 * ----------------------------------------------------------------------- */
typedef struct {
    float buf[PP_DELAY_MAX];
    int   idx;
    int   len;
    float fb;
    float lp;
} pp_symp_t;

/* ----------------------------------------------------------------------- *
 *  Schroeder room reverb (4 combs + 2 allpass), stereo
 * ----------------------------------------------------------------------- */
#define PP_RVB_COMBS 4
#define PP_RVB_APS   2
#define PP_RVB_MAXLEN 2400
typedef struct {
    float cbuf[2][PP_RVB_COMBS][PP_RVB_MAXLEN];
    int   clen[2][PP_RVB_COMBS];
    int   cidx[2][PP_RVB_COMBS];
    float clp[2][PP_RVB_COMBS];       /* comb damping                      */
    float abuf[2][PP_RVB_APS][PP_RVB_MAXLEN];
    int   alen[2][PP_RVB_APS];
    int   aidx[2][PP_RVB_APS];
    float predelay[2][PP_RVB_MAXLEN]; /* "far away" pre-delay              */
    int   pdlen;
    int   pdidx[2];
    float wet_lp[2];                  /* distance lowpass                  */
} pp_reverb_t;

/* ----------------------------------------------------------------------- *
 *  Top-level state
 * ----------------------------------------------------------------------- */
struct pp_state {
    float sr;

    /* knob values, normalised 0..1 (except polyphony) */
    float p[PP_P_COUNT];
    int   polyphony;       /* 1..16                                        */

    pp_voice_t voices[PP_MAX_VOICES];
    int   age_counter;

    pp_symp_t symp[PP_SYMPATHETIC_TAPS];
    float symp_send;

    pp_reverb_t rvb;

    /* soundboard body resonance: a modal bank approximating a piano's
     * soundboard/body response, adding the wood/air a bare string lacks */
    float body_y1[PP_BODY_MODES], body_y2[PP_BODY_MODES];
    float body_b1[PP_BODY_MODES], body_b2[PP_BODY_MODES], body_a[PP_BODY_MODES];
    float body_g[PP_BODY_MODES];

    /* master DC blocker (belt-and-suspenders after the reverb) */
    float dcx_l, dcy_l, dcx_r, dcy_r;

    /* scratch buffer for building note excitations (avoids stack pressure) */
    float scratch[PP_DELAY_MAX];

    uint32_t rng;          /* master RNG (note-on randomisation, RNDMZ)    */
};

/* ----------------------------------------------------------------------- *
 *  Soundboard body resonance (a few broad 2-pole resonators)
 * ----------------------------------------------------------------------- */
static void body_init(pp_state_t *st) {
    /* A modal bank spread across a piano soundboard's radiating range: low
     * modes broad and strong, upper modes tighter and quieter, so the body
     * colours the low-mid (wood/air) without a metallic ring. */
    static const float freqs[PP_BODY_MODES] =
        {  75.0f, 110.0f, 160.0f, 220.0f, 300.0f, 420.0f, 580.0f, 800.0f, 1150.0f, 1900.0f };
    static const float rs[PP_BODY_MODES] =
        { 0.86f, 0.87f, 0.88f, 0.89f, 0.90f, 0.91f, 0.90f, 0.88f, 0.85f, 0.80f };
    static const float gs[PP_BODY_MODES] =
        { 1.00f, 1.00f, 0.92f, 0.85f, 0.78f, 0.68f, 0.58f, 0.44f, 0.30f, 0.18f };
    for (int i = 0; i < PP_BODY_MODES; i++) {
        float w = 2.0f * (float)M_PI * freqs[i] / st->sr;
        float r = rs[i];
        st->body_b1[i] = 2.0f * r * cosf(w);
        st->body_b2[i] = -r * r;
        st->body_a[i]  = (1.0f - r);        /* rough bandpass normalisation   */
        st->body_g[i]  = gs[i];
        st->body_y1[i] = st->body_y2[i] = 0.0f;
    }
}

/* Run the mono bus through the modal soundboard; returns the added colour. */
static inline float body_process(pp_state_t *st, float x) {
    float out = 0.0f;
    for (int i = 0; i < PP_BODY_MODES; i++) {
        float y = st->body_a[i] * x
                + st->body_b1[i] * st->body_y1[i]
                + st->body_b2[i] * st->body_y2[i];
        st->body_y2[i] = st->body_y1[i];
        st->body_y1[i] = y;
        out += st->body_g[i] * y;
    }
    return out;
}

/* ----------------------------------------------------------------------- *
 *  Reverb construction / processing
 * ----------------------------------------------------------------------- */
static void reverb_init(pp_reverb_t *r, float sr) {
    memset(r, 0, sizeof(*r));
    /* prime-ish comb lengths (Freeverb-inspired) scaled from 44.1k */
    static const int base_c[PP_RVB_COMBS] = { 1557, 1617, 1491, 1422 };
    static const int base_a[PP_RVB_APS]   = { 225, 556 };
    float k = sr / 44100.0f;
    for (int ch = 0; ch < 2; ch++) {
        int spread = ch ? 23 : 0;   /* stereo decorrelation */
        for (int c = 0; c < PP_RVB_COMBS; c++) {
            int L = (int)(base_c[c] * k) + spread;
            if (L < 1) L = 1;
            if (L >= PP_RVB_MAXLEN) L = PP_RVB_MAXLEN - 1;
            r->clen[ch][c] = L;
        }
        for (int a = 0; a < PP_RVB_APS; a++) {
            int L = (int)(base_a[a] * k) + spread;
            if (L < 1) L = 1;
            if (L >= PP_RVB_MAXLEN) L = PP_RVB_MAXLEN - 1;
            r->alen[ch][a] = L;
        }
    }
    r->pdlen = (int)(0.010f * sr);   /* up to 10 ms predelay when far      */
    if (r->pdlen >= PP_RVB_MAXLEN) r->pdlen = PP_RVB_MAXLEN - 1;
}

/* wet in [0,1]; also drives "distance": more wet -> darker + pre-delayed */
static void reverb_process(pp_reverb_t *r, float in_l, float in_r,
                           float wet, float *out_l, float *out_r) {
    const float fb   = 0.80f + 0.03f * wet;       /* longer tail when wet   */
    const float damp = 0.20f + 0.25f * wet;       /* darker when far        */
    const float in[2] = { in_l, in_r };
    float out[2];

    for (int ch = 0; ch < 2; ch++) {
        /* pre-delay grows with wet for a "far away" feel */
        int pd = (int)(r->pdlen * wet);
        float x = in[ch];
        if (pd > 0) {
            r->predelay[ch][r->pdidx[ch]] = x;
            int rd = r->pdidx[ch] - pd;
            if (rd < 0) rd += r->pdlen ? r->pdlen : 1;
            float dv = r->predelay[ch][rd];
            r->pdidx[ch]++; if (r->pdidx[ch] >= r->pdlen) r->pdidx[ch] = 0;
            x = dv;
        }

        float acc = 0.0f;
        for (int c = 0; c < PP_RVB_COMBS; c++) {
            int L = r->clen[ch][c];
            float y = r->cbuf[ch][c][r->cidx[ch][c]];
            /* comb damping lowpass in the feedback path */
            r->clp[ch][c] = pp_lerp(y, r->clp[ch][c], damp);
            float fbv = x + r->clp[ch][c] * fb;
            r->cbuf[ch][c][r->cidx[ch][c]] = fbv;
            r->cidx[ch][c]++; if (r->cidx[ch][c] >= L) r->cidx[ch][c] = 0;
            acc += y;
        }
        acc *= (1.0f / PP_RVB_COMBS);

        for (int a = 0; a < PP_RVB_APS; a++) {
            int L = r->alen[ch][a];
            float bufv = r->abuf[ch][a][r->aidx[ch][a]];
            float y = -acc + bufv;
            r->abuf[ch][a][r->aidx[ch][a]] = acc + bufv * 0.5f;
            r->aidx[ch][a]++; if (r->aidx[ch][a] >= L) r->aidx[ch][a] = 0;
            acc = y;
        }

        /* distance lowpass on the wet signal */
        r->wet_lp[ch] = pp_lerp(acc, r->wet_lp[ch], 0.15f * wet);
        out[ch] = r->wet_lp[ch] * 0.6f;   /* headroom on the wet tail */
    }

    /* equal-ish power dry/wet, and pull dry down as we go fully wet */
    float w = wet;
    float d = 1.0f - wet;
    *out_l = in_l * d + out[0] * w;
    *out_r = in_r * d + out[1] * w;
}

/* ----------------------------------------------------------------------- *
 *  Sympathetic bank
 * ----------------------------------------------------------------------- */
static void symp_init(pp_state_t *st) {
    /* Tune the open-string bank across a musical spread (roughly a stack of
     * fifths/octaves) so struck notes excite plausible neighbours. */
    static const float semis[PP_SYMPATHETIC_TAPS] =
        { -24, -17, -12, -5, 0, 7, 12, 19, 24 };
    for (int i = 0; i < PP_SYMPATHETIC_TAPS; i++) {
        float f = 220.0f * powf(2.0f, semis[i] / 12.0f);
        int L = (int)(st->sr / f + 0.5f);
        if (L < 2) L = 2;
        if (L >= PP_DELAY_MAX) L = PP_DELAY_MAX - 1;
        memset(st->symp[i].buf, 0, sizeof(st->symp[i].buf));
        st->symp[i].idx = 0;
        st->symp[i].len = L;
        st->symp[i].fb  = 0.990f;     /* long ring, these are un-damped     */
        st->symp[i].lp  = 0.0f;
    }
}

/* ----------------------------------------------------------------------- *
 *  Voice helpers
 * ----------------------------------------------------------------------- */
static float mtof(int note) {   /* MIDI note -> Hz */
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

/* Recompute the amplitude/brightness coefficients that CAN change mid-note
 * (felt damping, decay ring time, gauge brightness) from the current global
 * knobs, without touching delay/dispersion (which would shift pitch). Called
 * at note-on and again whenever Felt/Decay/Gauge move, so a held note responds
 * live to those knobs. */
static void voice_update_tone(pp_state_t *st, pp_voice_t *v) {
    float felt  = st->p[PP_P_FELT];
    float decay = st->p[PP_P_DECAY];
    float gauge = st->p[PP_P_GAUGE];

    v->damp_coef = pp_clampf(0.15f + 0.75f * felt + 0.15f * gauge, 0.0f, 0.93f);

    /* Decay knob -> ring time. The in-loop damping filter removes energy on
     * top of the loop gain, so we aim the raw T60 a bit high (~14 s) to land a
     * ~10 s audible tail at full Decay. Felt shortens it. */
    float t60 = pp_lerp(0.08f, 14.0f, decay * decay);
    t60 *= pp_lerp(1.0f, 0.12f, felt);
    float delay_s = v->str[0].delay / st->sr;
    float g = powf(10.0f, -3.0f * delay_s / t60);
    g = pp_clampf(g, 0.0f, 0.99995f);
    v->loop_gain = g;

    /* Double decay = gentle bridge coupling (voice_tick) drains the loud common
     * mode fast, while the differential strings, given slightly lower loss here,
     * bleed to the bridge slowly and ring on as the quiet aftersound. */
    static const float after[PP_MAX_STRINGS] = { 0.0f, 0.07f, 0.11f };
    for (int i = 0; i < v->nstr; i++)
        v->str[i].gain = pp_clampf(g + (1.0f - g) * after[i], 0.0f, 0.99995f);
}

static void voice_silence(pp_voice_t *v) {
    v->active = 0; v->held = 0; v->quiet = 0;
    v->bow_lp = 0.0f; v->exc_len = 0;
    v->amp_env = 1.0f; v->amp_inc = 1.0f;
    v->nstr = 1; v->out_scale = 1.0f;
    for (int sidx = 0; sidx < PP_MAX_STRINGS; sidx++) {
        pp_string_t *s = &v->str[sidx];
        s->widx = 0; s->delay = 100.0f; s->gain = 0.0f;
        s->damp_z = s->dc_x = s->dc_y = 0.0f;
        for (int q = 0; q < PP_DISP_STAGES; q++) { s->ap_x[q] = s->ap_z[q] = 0.0f; }
        memset(s->buf, 0, sizeof(s->buf));
    }
    for (int i = 0; i < PP_MAX_PREP; i++) v->prep[i].active = 0;
}

/* Map the global knobs into per-voice coefficients at note-on time. */
static void voice_configure(pp_state_t *st, pp_voice_t *v, int note, int vel) {
    v->note   = note;
    v->freq   = mtof(note);
    v->vel    = pp_clampf(vel / 127.0f, 0.0f, 1.0f);
    v->active = 1;
    v->held   = 1;
    v->age    = st->age_counter++;
    v->rng    = pp_xs(&st->rng);

    float felt   = st->p[PP_P_FELT];
    float attack = st->p[PP_P_ATTACK];
    float gauge  = st->p[PP_P_GAUGE];
    float hammer = st->p[PP_P_HAMMER];
    float disturb= st->p[PP_P_DISTURB];

    /* --- damping brightness (felt + gauge) ---
     * One-pole lowpass in the loop. More felt / heavier gauge = darker and a
     * faster high-frequency roll-off (the classic "muted" prepared-piano tone). */
    v->damp_coef = pp_clampf(0.15f + 0.75f * felt + 0.15f * gauge, 0.0f, 0.93f);

    /* --- dispersion / inharmonicity (stiff-string partial stretching) ---
     * Real pianos are least inharmonic in the middle and stiffer (more
     * inharmonic) toward the bass and, especially, the treble. Model that
     * register curve, add the Gauge (thickness) contribution, and drive a
     * cascade of allpasses (PP_DISP_STAGES) for a stretched, singing partial
     * series instead of a single crude bend. */
    {
        float bass   = pp_clampf((48.0f - note) / 40.0f, 0.0f, 1.0f);   /* .. low  */
        float treble = pp_clampf((note - 72.0f) / 36.0f, 0.0f, 1.0f);   /* high .. */
        float inh = 0.10f + 0.30f * bass + 0.45f * treble * treble;
        /* Subtle stiffness colour from register + gauge. (True calibrated
         * inharmonic *stretch* needs a proper multi-section dispersion filter
         * and allpass interpolation -- a dedicated pass; see DESIGN roadmap.) */
        v->disp_coef = -pp_clampf(0.06f + 0.30f * inh + 0.42f * gauge, 0.0f, 0.5f);
    }

    /* --- string length / tuning ---
     * Heavier gauge lowers tension slightly (a touch flatter) and, more
     * importantly, stiffens the string (more inharmonicity via dispersion).
     * Compensate the delay for the phase delay added by the loop lowpass and
     * the dispersion allpass so notes stay in tune as felt/gauge change. */
    float freq = v->freq * (1.0f - 0.010f * gauge);
    float gd_lp = v->damp_coef / (1.0f - v->damp_coef);   /* one-pole grp delay */
    float gd_ap = PP_DISP_STAGES * 2.0f * (-v->disp_coef);/* allpass grp delay  */
    float base_delay = st->sr / freq - 1.0f - gd_lp - gd_ap;
    if (base_delay < 2.0f)  base_delay = 2.0f;
    if (base_delay > (float)(PP_DELAY_MAX - 2)) base_delay = (float)(PP_DELAY_MAX - 2);

    /* --- hammer excitation ---
     * Contact time: soft felt hammer = longer, rounder; metal = short click.
     * Brightness of the burst rises with the hammer knob and with velocity. */
    float contact_ms = pp_lerp(6.0f, 0.8f, hammer);     /* felt->metal */
    v->exc_len = (int)(contact_ms * 0.001f * st->sr) + 1;
    /* heavier gauge is "harder to strike": same key effort -> less energy in */
    v->exc_gain = (0.35f + 0.65f * v->vel) * pp_lerp(1.0f, 0.6f, gauge);

    /* --- unison strings for this note (1 bass .. 3 treble), each detuned a
     * couple of cents so the course beats -> shimmer + double-decay. --- */
    v->nstr = strings_for_note(note);
    if (v->nstr < 1) v->nstr = 1;
    if (v->nstr > PP_MAX_STRINGS) v->nstr = PP_MAX_STRINGS;
    v->out_scale = 1.0f / sqrtf((float)v->nstr);

    static const float cents3[3] = { 0.0f, 2.5f, -2.5f };
    static const float cents2[2] = { 0.0f, 2.8f };
    float softness = 1.0f - hammer;   /* 1 = soft felt, 0 = metal */

    for (int sidx = 0; sidx < v->nstr; sidx++) {
        pp_string_t *s = &v->str[sidx];
        float cents = (v->nstr == 3) ? cents3[sidx]
                    : (v->nstr == 2) ? cents2[sidx] : 0.0f;
        float sd = base_delay * powf(2.0f, -cents / 1200.0f);
        if (sd < 2.0f) sd = 2.0f;
        if (sd > (float)(PP_DELAY_MAX - 2)) sd = (float)(PP_DELAY_MAX - 2);
        s->delay = sd;
        int N = (int)sd + 1; if (N > PP_DELAY_MAX) N = PP_DELAY_MAX;
        s->widx = (int)sd; if (s->widx >= PP_DELAY_MAX) s->widx = PP_DELAY_MAX - 1;

        /* --- hammer strike excitation ---
         * Build a smooth raised-cosine hammer force pulse (its width sets the
         * spectral rolloff: wide/soft -> fundamental-heavy and mellow,
         * narrow/metal -> bright), add a little coloured noise for life, then
         * apply the strike-position comb (1 - z^-d), d ~ N/8. That comb imposes
         * the piano's sin(k*pi*beta) mode weighting and the signature
         * missing-7th notch -- the difference between a struck piano and the
         * plucked, hollow clav tone the old white-noise burst produced. */
        float *tmp = st->scratch;
        /* Dynamic hammer: a harder strike (higher velocity) compresses the felt,
         * shortening the contact -> a narrower pulse -> a brighter tone. So the
         * effective softness shrinks with velocity, and playing harder gets
         * brighter, not just louder (the key piano expressiveness). */
        float eff_soft = softness * pp_lerp(1.25f, 0.45f, v->vel);
        int hw = (int)((0.04f + 0.22f * eff_soft) * N) + 2;   /* pulse half-width */
        if (hw > N / 2) hw = N / 2;
        if (hw < 2) hw = 2;
        for (int i = 0; i < N; i++) tmp[i] = 0.0f;
        for (int j = -hw; j <= hw; j++) {
            int idx = ((j % N) + N) % N;
            tmp[idx] += 0.5f * (1.0f + cosf((float)M_PI * j / hw));
        }
        float nz_amt = 0.05f + 0.22f * hammer;                /* metal = livelier */
        float ncoef  = pp_lerp(0.65f, 0.10f, hammer);
        float nlp = 0.0f;
        for (int i = 0; i < N; i++) {
            float nz = pp_frand_bi(&v->rng);
            nlp = pp_lerp(nz, nlp, ncoef);
            tmp[i] += nlp * nz_amt;
        }
        int d = (int)(0.125f * N + 0.5f);                     /* strike ~ 1/8    */
        if (d < 1) d = 1;
        if (d >= N) d = N - 1;
        float norm = v->exc_gain * (5.5f / (float)hw);        /* level normalise */
        for (int i = 0; i < PP_DELAY_MAX; i++) s->buf[i] = 0.0f;
        for (int i = 0; i < N; i++) {
            int pidx = i - d; if (pidx < 0) pidx += N;
            s->buf[i] = (tmp[i] - tmp[pidx]) * norm;
        }
        s->damp_z = s->dc_x = s->dc_y = 0.0f;
        for (int q = 0; q < PP_DISP_STAGES; q++) { s->ap_x[q] = s->ap_z[q] = 0.0f; }
    }

    /* --- decay / brightness (loop gain + damping + per-string aftersound) ---
     * Derived from Felt/Decay/Gauge; recomputed live when those knobs move. */
    voice_update_tone(st, v);

    /* --- Attack: gentle-bow amplitude swell. The knob sets a fade-in time
     * from 0 up to ~5 s, on top of the pluck->bow excitation morph. --- */
    float attack_s = attack * 5.0f;
    if (attack_s < 0.004f) {
        v->amp_env = 1.0f; v->amp_inc = 1.0f;           /* instant (plucked) */
    } else {
        v->amp_env = 0.0f; v->amp_inc = 1.0f / (attack_s * st->sr);
    }
    v->bow_amt = attack;   /* higher attack also drives the sustained bow */
    v->bow_lp  = 0.0f;

    /* --- keyboard panning: low strings sit left, high strings right. --- */
    {
        float t = pp_clampf((note - 21) / 87.0f, 0.0f, 1.0f);  /* A0..C8 */
        float pan = (t - 0.5f) * 0.7f;                         /* -0.35..0.35 */
        float ang = (pan * 0.5f + 0.5f) * (float)M_PI * 0.5f;
        v->pan_l = cosf(ang);
        v->pan_r = sinf(ang);
    }

    /* --- preparation objects laid on the string (applied to the main string
     * of the course). disturb sets how many prepared sources exist. --- */
    int nprep = (int)(disturb * PP_MAX_PREP + 0.5f);
    for (int i = 0; i < PP_MAX_PREP; i++) {
        pp_prep_t *pr = &v->prep[i];
        if (i < nprep && pp_frand(&v->rng) < (0.4f + 0.6f * disturb)) {
            pr->active   = 1;
            pr->thresh   = 0.02f + 0.25f * pp_frand(&v->rng);
            pr->buzz_amt = (0.3f + 0.7f * pp_frand(&v->rng)) * (0.4f + 0.6f * disturb);
            float rhz    = pp_lerp(600.0f, 5000.0f, pp_frand(&v->rng));
            pr->rate     = 2.0f * (float)M_PI * rhz / st->sr;
            pr->phase    = pp_frand(&v->rng) * 6.2831853f;
            pr->mute     = 0.001f + 0.02f * pp_frand(&v->rng);
            pr->lp       = 0.0f;
        } else {
            pr->active = 0;
        }
    }
}

/* Render one sample from a voice (a course of 1..3 strings), and add its
 * sympathetic send. */
static inline float voice_tick(pp_state_t *st, pp_voice_t *v, float *symp_in) {
    (void)st;
    float sum = 0.0f;
    float wr[PP_MAX_STRINGS];

    for (int sidx = 0; sidx < v->nstr; sidx++) {
        pp_string_t *s = &v->str[sidx];

        /* fractional read from this string's delay line */
        float rp = (float)s->widx - s->delay;
        while (rp < 0) rp += PP_DELAY_MAX;
        int i0 = (int)rp;
        float fr = rp - i0;
        int i1 = i0 + 1; if (i1 >= PP_DELAY_MAX) i1 -= PP_DELAY_MAX;
        float rdv = pp_lerp(s->buf[i0], s->buf[i1], fr);

        /* string damping (one-pole lowpass) */
        s->damp_z = pp_lerp(rdv, s->damp_z, v->damp_coef);
        float d = s->damp_z;

        /* dispersion cascade -> stiff-string inharmonic partial stretching */
        float ap = d;
        for (int q = 0; q < PP_DISP_STAGES; q++) {
            float y = v->disp_coef * ap + s->ap_x[q] - v->disp_coef * s->ap_z[q];
            s->ap_x[q] = ap;
            s->ap_z[q] = y;
            ap = y;
        }

        float loop = ap * s->gain;

        /* preparation rattle -- applied to the main string of the course */
        if (sidx == 0) {
            float buzz = 0.0f;
            for (int i = 0; i < PP_MAX_PREP; i++) {
                pp_prep_t *pr = &v->prep[i];
                if (!pr->active) continue;
                float a = loop >= 0 ? loop : -loop;
                if (a > pr->thresh) {
                    pr->phase += pr->rate;
                    if (pr->phase > 6.2831853f) pr->phase -= 6.2831853f;
                    float r = pp_tanhf(sinf(pr->phase) * 3.0f);
                    pr->lp = pp_lerp(r * a, pr->lp, 0.3f);
                    buzz += pr->lp * pr->buzz_amt;
                    loop *= (1.0f - pr->mute);
                }
            }
            loop += buzz * 0.6f;
        }

        /* bow / sustained excitation while held */
        if (v->held && v->bow_amt > 0.001f) {
            float nz = pp_frand_bi(&v->rng);
            v->bow_lp = pp_lerp(nz, v->bow_lp, 0.6f);
            /* bow_amt drives a clearly-audible sustain: even at 50% attack the
             * string should sing, not just whisper under the decay. */
            float drive = v->bow_amt * (0.4f + 0.6f * v->vel);
            float head = 0.25f - (loop >= 0 ? loop : -loop);   /* cap bowed level */
            if (head < 0.0f) head = 0.0f;
            loop += (v->bow_lp * 0.5f + 0.5f * pp_tanhf(loop * 2.0f)) * drive * 0.35f * head;
        }

        /* DC blocker in the feedback path */
        float hp = loop - s->dc_x + 0.999f * s->dc_y;
        s->dc_x = loop; s->dc_y = hp; loop = hp;

        wr[sidx] = loop;   /* defer writeback until after bridge coupling */
        sum += d;
    }

    /* --- bridge coupling: the unison strings share a bridge, so their common
     * (in-phase) motion loads it and sheds energy fast, while opposed motions
     * barely move the bridge and ring on -- the real piano "double decay". --- */
    if (v->nstr > 1) {
        float bavg = 0.0f;
        for (int i = 0; i < v->nstr; i++) bavg += wr[i];
        bavg *= (1.0f / (float)v->nstr);
        for (int i = 0; i < v->nstr; i++) wr[i] -= PP_COUPLING * bavg;
    }
    for (int i = 0; i < v->nstr; i++) {
        pp_string_t *s = &v->str[i];
        s->buf[s->widx] = wr[i];
        s->widx++; if (s->widx >= PP_DELAY_MAX) s->widx = 0;
    }

    /* normalise for the string count, then apply the attack swell */
    float out = sum * v->out_scale;
    if (v->amp_env < 1.0f) {
        v->amp_env += v->amp_inc;
        if (v->amp_env > 1.0f) v->amp_env = 1.0f;
    }
    out *= v->amp_env;

    /* send to sympathetic bus (scaled outside by symp_send) */
    *symp_in += out;

    /* cull: released and quiet for a short while */
    float a = out >= 0 ? out : -out;
    if (!v->held && a < 5.0e-5f) {
        if (++v->quiet > 2400) v->age = -1;   /* ~55 ms below floor */
    } else {
        v->quiet = 0;
    }
    return out;
}

/* ----------------------------------------------------------------------- *
 *  Voice allocation
 * ----------------------------------------------------------------------- */
static pp_voice_t *alloc_voice(pp_state_t *st, int note) {
    int limit = st->polyphony;
    if (limit < 1) limit = 1;
    if (limit > PP_MAX_VOICES) limit = PP_MAX_VOICES;

    /* mono / limited: retrigger a voice already on this note first */
    for (int i = 0; i < limit; i++)
        if (st->voices[i].active && st->voices[i].note == note)
            return &st->voices[i];

    /* find a free slot within the polyphony limit */
    for (int i = 0; i < limit; i++)
        if (!st->voices[i].active)
            return &st->voices[i];

    /* steal the oldest active voice within the limit */
    int oldest = 0; int best = 0x7fffffff;
    for (int i = 0; i < limit; i++) {
        int a = st->voices[i].age;
        if (a >= 0 && a < best) { best = a; oldest = i; }
    }
    voice_silence(&st->voices[oldest]);
    return &st->voices[oldest];
}

/* ----------------------------------------------------------------------- *
 *  Public API
 * ----------------------------------------------------------------------- */
pp_state_t *pp_create(float sample_rate) {
    pp_state_t *st = (pp_state_t *)calloc(1, sizeof(pp_state_t));
    if (!st) return NULL;
    st->sr = sample_rate > 0 ? sample_rate : 44100.0f;
    st->rng = 0xC0FFEEu;

    /* sensible defaults: a plain, medium-decay felt piano */
    st->p[PP_P_FELT]        = 0.25f;
    st->p[PP_P_ATTACK]      = 0.0f;
    st->p[PP_P_DECAY]       = 0.55f;
    st->p[PP_P_GAUGE]       = 0.2f;
    st->p[PP_P_HAMMER]      = 0.28f;
    st->p[PP_P_SYMPATHETIC] = 0.25f;
    st->p[PP_P_DISTURB]     = 0.0f;
    st->p[PP_P_REVERB]      = 0.25f;
    st->polyphony           = PP_MAX_VOICES;
    st->symp_send           = st->p[PP_P_SYMPATHETIC];

    for (int i = 0; i < PP_MAX_VOICES; i++) voice_silence(&st->voices[i]);
    symp_init(st);
    reverb_init(&st->rvb, st->sr);
    body_init(st);
    return st;
}

void pp_destroy(pp_state_t *st) { if (st) free(st); }

void pp_reset(pp_state_t *st) {
    if (!st) return;
    for (int i = 0; i < PP_MAX_VOICES; i++) voice_silence(&st->voices[i]);
    symp_init(st);
    reverb_init(&st->rvb, st->sr);
    body_init(st);
}

void pp_seed(pp_state_t *st, uint32_t seed) { if (st) st->rng = seed ? seed : 1u; }

void pp_set_param(pp_state_t *st, pp_param_t id, float value) {
    if (!st || id < 0 || id >= PP_P_COUNT) return;
    switch (id) {
        case PP_P_POLYPHONY: {
            int n = (int)(value + 0.5f);
            if (n < 1) n = 1;
            if (n > PP_MAX_VOICES) n = PP_MAX_VOICES;
            st->polyphony = n;
            break;
        }
        case PP_P_RNDMZ:
            if (value >= 0.5f) pp_randomize(st);   /* only on the "Randomize!" pick */
            break;
        case PP_P_SYMPATHETIC:
            st->p[id] = pp_clampf(value, 0.0f, 1.0f);
            st->symp_send = st->p[id];
            break;
        default:
            st->p[id] = pp_clampf(value, 0.0f, 1.0f);
            /* Felt/Decay/Gauge change damping and ring time, which a currently
             * held note can respond to without a pitch jump -> live tweak. */
            if (id == PP_P_FELT || id == PP_P_DECAY || id == PP_P_GAUGE) {
                for (int i = 0; i < PP_MAX_VOICES; i++)
                    if (st->voices[i].active) voice_update_tone(st, &st->voices[i]);
            }
            break;
    }
}

float pp_get_param(pp_state_t *st, pp_param_t id) {
    if (!st || id < 0 || id >= PP_P_COUNT) return 0.0f;
    if (id == PP_P_POLYPHONY) return (float)st->polyphony;
    return st->p[id];
}

void pp_note_on(pp_state_t *st, int note, int vel) {
    if (!st || note < 0 || note > 127) return;
    if (vel <= 0) { pp_note_off(st, note); return; }
    pp_voice_t *v = alloc_voice(st, note);
    voice_configure(st, v, note, vel);
}

void pp_note_off(pp_state_t *st, int note) {
    if (!st) return;
    for (int i = 0; i < PP_MAX_VOICES; i++)
        if (st->voices[i].active && st->voices[i].note == note)
            st->voices[i].held = 0;   /* let it ring / stop bowing */
}

void pp_all_notes_off(pp_state_t *st) {
    if (!st) return;
    for (int i = 0; i < PP_MAX_VOICES; i++) st->voices[i].held = 0;
}

void pp_randomize(pp_state_t *st) {
    if (!st) return;
    /* Randomise the eight continuous knobs. Bias a little toward musical
     * territory: keep decay from being ultra-short, keep reverb sane. */
    st->p[PP_P_FELT]        = pp_frand(&st->rng);
    st->p[PP_P_ATTACK]      = pp_frand(&st->rng);
    st->p[PP_P_DECAY]       = 0.2f + 0.8f * pp_frand(&st->rng);
    st->p[PP_P_GAUGE]       = pp_frand(&st->rng);
    st->p[PP_P_HAMMER]      = pp_frand(&st->rng);
    st->p[PP_P_SYMPATHETIC] = pp_frand(&st->rng);
    st->p[PP_P_DISTURB]     = pp_frand(&st->rng) * pp_frand(&st->rng); /* skew low */
    st->p[PP_P_REVERB]      = 0.6f * pp_frand(&st->rng);
    st->symp_send           = st->p[PP_P_SYMPATHETIC];
}

void pp_render_float(pp_state_t *st, float *out_l, float *out_r, int n) {
    if (!st) return;
    float symp_send = st->symp_send;
    for (int f = 0; f < n; f++) {
        float mix_l = 0.0f, mix_r = 0.0f;
        float symp_in = 0.0f;

        for (int i = 0; i < PP_MAX_VOICES; i++) {
            pp_voice_t *v = &st->voices[i];
            if (!v->active) continue;
            float o = voice_tick(st, v, &symp_in);
            mix_l += o * v->pan_l;
            mix_r += o * v->pan_r;
            if (v->age < 0) voice_silence(v);   /* culled */
        }

        /* --- sympathetic resonance bank --- */
        float symp_out = 0.0f;
        if (symp_send > 0.001f) {
            float drive = symp_in * symp_send * 0.10f;
            for (int i = 0; i < PP_SYMPATHETIC_TAPS; i++) {
                pp_symp_t *r = &st->symp[i];
                float y = r->buf[r->idx];
                r->lp = pp_lerp(y, r->lp, 0.12f);
                float fbv = drive + r->lp * r->fb;
                r->buf[r->idx] = fbv;
                r->idx++; if (r->idx >= r->len) r->idx = 0;
                symp_out += y;
            }
            symp_out *= (1.0f / PP_SYMPATHETIC_TAPS) * symp_send;
        }
        mix_l += symp_out;
        mix_r += symp_out;

        /* soundboard body resonance: colour the mono sum and fold it back in,
         * adding the wood/air a bare string lacks */
        float body = body_process(st, (mix_l + mix_r) * 0.5f) * 0.05f;
        mix_l += body;
        mix_r += body;

        /* headroom / gentle bus saturation before reverb, then master trim */
        mix_l = pp_tanhf(mix_l * 0.5f) * 0.85f;
        mix_r = pp_tanhf(mix_r * 0.5f) * 0.85f;

        float l, r;
        reverb_process(&st->rvb, mix_l, mix_r, st->p[PP_P_REVERB], &l, &r);

        /* master DC blocker */
        float hl = l - st->dcx_l + 0.999f * st->dcy_l; st->dcx_l = l; st->dcy_l = hl;
        float hr = r - st->dcx_r + 0.999f * st->dcy_r; st->dcx_r = r; st->dcy_r = hr;

        out_l[f] = hl * 1.3f;
        out_r[f] = hr * 1.3f;
    }
}

void pp_render_int16(pp_state_t *st, int16_t *out_lr, int n) {
    if (!st) return;
    /* render in small chunks to a stack buffer, then convert/clip */
    enum { CH = 128 };
    float l[CH], r[CH];
    int done = 0;
    while (done < n) {
        int m = n - done; if (m > CH) m = CH;
        pp_render_float(st, l, r, m);
        for (int i = 0; i < m; i++) {
            float fl = pp_clampf(l[i], -1.0f, 1.0f);
            float fr = pp_clampf(r[i], -1.0f, 1.0f);
            out_lr[(done + i) * 2 + 0] = (int16_t)lrintf(fl * 32767.0f);
            out_lr[(done + i) * 2 + 1] = (int16_t)lrintf(fr * 32767.0f);
        }
        done += m;
    }
}
