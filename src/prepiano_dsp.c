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
 *  Voice (one struck string)
 * ----------------------------------------------------------------------- */
typedef struct {
    int   active;
    int   note;
    int   age;             /* for voice stealing (older = higher)          */
    float vel;             /* 0..1                                          */
    float freq;

    /* waveguide delay line */
    float buf[PP_DELAY_MAX];
    int   widx;
    float delay;           /* fractional delay in samples                  */

    /* loop filters */
    float damp_z;          /* one-pole lowpass (string damping) state       */
    float ap_z;            /* first-order allpass (dispersion) state        */
    float ap_x;            /* allpass input history                         */

    /* per-voice tuned amounts (captured at note-on from global knobs) */
    float loop_gain;       /* < 1, sets decay                              */
    float damp_coef;       /* 0..~0.95, string brightness/felt            */
    float disp_coef;       /* allpass coefficient, string stiffness       */
    float pan_l, pan_r;    /* equal-power keyboard pan gains              */

    /* excitation */
    int   exc_len;         /* remaining samples of the hammer burst        */
    float exc_lp;          /* hammer noise colouring state                 */
    float exc_gain;
    float bow_amt;         /* 0 = pluck, 1 = sustained bow                 */
    float bow_lp;          /* bow noise colouring                          */
    int   held;            /* note is still down (bow keeps exciting)      */

    /* preparation objects attached to this string */
    pp_prep_t prep[PP_MAX_PREP];

    uint32_t rng;
} pp_voice_t;

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

    uint32_t rng;          /* master RNG (note-on randomisation, RNDMZ)    */
};

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

static void voice_silence(pp_voice_t *v) {
    v->active = 0; v->held = 0;
    v->damp_z = v->ap_z = v->ap_x = 0.0f;
    v->exc_len = 0; v->exc_lp = 0.0f; v->bow_lp = 0.0f;
    memset(v->buf, 0, sizeof(v->buf));
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
    float decay  = st->p[PP_P_DECAY];
    float gauge  = st->p[PP_P_GAUGE];
    float hammer = st->p[PP_P_HAMMER];
    float disturb= st->p[PP_P_DISTURB];

    /* --- damping brightness (felt + gauge) ---
     * One-pole lowpass in the loop. More felt / heavier gauge = darker and a
     * faster high-frequency roll-off (the classic "muted" prepared-piano tone). */
    v->damp_coef = pp_clampf(0.15f + 0.75f * felt + 0.15f * gauge, 0.0f, 0.93f);

    /* --- dispersion (stiffness / gauge -> inharmonic, bell-like partials) --- */
    v->disp_coef = -pp_clampf(0.06f + 0.42f * gauge, 0.0f, 0.5f);

    /* --- string length / tuning ---
     * Heavier gauge lowers tension slightly (a touch flatter) and, more
     * importantly, stiffens the string (more inharmonicity via dispersion).
     * Compensate the delay for the phase delay added by the loop lowpass and
     * the dispersion allpass so notes stay in tune as felt/gauge change. */
    float freq = v->freq * (1.0f - 0.010f * gauge);
    float gd_lp = v->damp_coef / (1.0f - v->damp_coef);   /* one-pole grp delay */
    float gd_ap = 2.0f * (-v->disp_coef);                 /* allpass grp delay  */
    v->delay = st->sr / freq - 1.0f - gd_lp - gd_ap;
    if (v->delay < 2.0f)  v->delay = 2.0f;
    if (v->delay > (float)(PP_DELAY_MAX - 2)) v->delay = (float)(PP_DELAY_MAX - 2);

    /* --- decay (loop gain) ---
     * Map decay knob to a target T60 from ~0.15 s to ~18 s. Felt shortens it.
     * loop_gain = 10^(-3 * delaySeconds / T60). */
    float t60 = pp_lerp(0.15f, 18.0f, decay * decay);
    t60 *= pp_lerp(1.0f, 0.12f, felt);          /* felt = practice-strip mute */
    float delay_s = v->delay / st->sr;
    float g = powf(10.0f, -3.0f * delay_s / t60);
    v->loop_gain = pp_clampf(g, 0.0f, 0.99995f);

    /* --- hammer excitation ---
     * Contact time: soft felt hammer = longer, rounder; metal = short click.
     * Brightness of the burst rises with the hammer knob and with velocity. */
    float contact_ms = pp_lerp(6.0f, 0.8f, hammer);     /* felt->metal */
    v->exc_len = (int)(contact_ms * 0.001f * st->sr) + 1;
    v->exc_lp  = 0.0f;
    /* heavier gauge is "harder to strike": same key effort -> less energy in */
    v->exc_gain = (0.35f + 0.65f * v->vel) * pp_lerp(1.0f, 0.6f, gauge);

    /* --- keyboard panning: low strings sit left, high strings right, like
     * sitting at the instrument. Subtle (+-~0.35) and equal-power. --- */
    {
        float t = pp_clampf((note - 21) / 87.0f, 0.0f, 1.0f);  /* A0..C8 */
        float pan = (t - 0.5f) * 0.7f;                         /* -0.35..0.35 */
        float ang = (pan * 0.5f + 0.5f) * (float)M_PI * 0.5f;
        v->pan_l = cosf(ang);
        v->pan_r = sinf(ang);
    }

    /* --- attack morph: pluck (0) -> bow (1) ---
     * A bow keeps feeding the string while the key is held and rings on. */
    v->bow_amt = attack;
    v->bow_lp  = 0.0f;

    /* --- preparation objects laid on this specific string ---
     * disturb sets how many prepared sources exist system-wide; each struck
     * string randomly gets some of them, with random rattle character. */
    int nprep = (int)(disturb * PP_MAX_PREP + 0.5f);
    for (int i = 0; i < PP_MAX_PREP; i++) {
        pp_prep_t *pr = &v->prep[i];
        /* probability a given object lands on this string scales with disturb */
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

    /* --- excite the string: fill the whole loop with a coloured noise burst
     * shaped like a hammer strike (a raised-cosine window on noise). --- */
    int N = (int)(v->delay) + 1;
    if (N > PP_DELAY_MAX) N = PP_DELAY_MAX;
    /* Start the write pointer one delay-length ahead of the excitation so the
     * read pointer (widx - delay) traverses the freshly-written burst before
     * the feedback path overwrites it. Writing at widx==0 would clobber the
     * excitation before it is ever read (silent plucks). */
    v->widx = (int)v->delay;
    if (v->widx >= PP_DELAY_MAX) v->widx = PP_DELAY_MAX - 1;
    float hammer_lp = pp_lerp(0.85f, 0.05f, hammer); /* soft=darker burst */
    float lp = 0.0f;
    int burst = v->exc_len < N ? v->exc_len : N;
    for (int i = 0; i < PP_DELAY_MAX; i++) v->buf[i] = 0.0f;
    for (int i = 0; i < N; i++) {
        float e = 0.0f;
        if (i < burst) {
            float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / burst); /* window */
            float nz = pp_frand_bi(&v->rng);
            lp = pp_lerp(nz, lp, hammer_lp);
            /* metal hammer adds a hard bright click at the very front */
            float click = (i < 2) ? (hammer * (1.0f - hammer_lp)) : 0.0f;
            e = (lp + click) * w;
        }
        v->buf[i] = e * v->exc_gain;
    }
    v->damp_z = 0.0f; v->ap_z = 0.0f; v->ap_x = 0.0f;
}

/* Render one sample from a voice, and add its sympathetic send. */
static inline float voice_tick(pp_state_t *st, pp_voice_t *v, float *symp_in) {
    (void)st;   /* reserved for future global-coupled processing */
    /* fractional read from the delay line */
    float rp = (float)v->widx - v->delay;
    while (rp < 0) rp += PP_DELAY_MAX;
    int i0 = (int)rp;
    float fr = rp - i0;
    int i1 = i0 + 1; if (i1 >= PP_DELAY_MAX) i1 -= PP_DELAY_MAX;
    float s = pp_lerp(v->buf[i0], v->buf[i1], fr);

    /* string damping (one-pole lowpass): brighter when coef is low */
    v->damp_z = pp_lerp(s, v->damp_z, v->damp_coef);
    float d = v->damp_z;

    /* dispersion: first-order allpass -> inharmonic, stiff-string partials */
    float ap = v->disp_coef * d + v->ap_x - v->disp_coef * v->ap_z;
    v->ap_x = d;
    v->ap_z = ap;

    float loop = ap * v->loop_gain;

    /* --- preparation: prepared objects buzzing against the string --- */
    float buzz = 0.0f;
    for (int i = 0; i < PP_MAX_PREP; i++) {
        pp_prep_t *pr = &v->prep[i];
        if (!pr->active) continue;
        float a = loop >= 0 ? loop : -loop;
        if (a > pr->thresh) {
            /* the object chatters against the moving string: a fast rattle
             * gated by the string amplitude, coloured metallic, plus it
             * saps a little energy (dead weight) -> shortens/detunes tone */
            pr->phase += pr->rate;
            if (pr->phase > 6.2831853f) pr->phase -= 6.2831853f;
            float r = sinf(pr->phase);
            r = pp_tanhf(r * 3.0f);                 /* squared-off, buzzy */
            pr->lp = pp_lerp(r * a, pr->lp, 0.3f);
            buzz += pr->lp * pr->buzz_amt;
            loop *= (1.0f - pr->mute);
        }
    }
    loop += buzz * 0.6f;

    /* --- bow / sustained excitation while held (attack morph) --- */
    if (v->held && v->bow_amt > 0.001f) {
        float nz = pp_frand_bi(&v->rng);
        v->bow_lp = pp_lerp(nz, v->bow_lp, 0.6f);
        /* stick-slip-ish: bow drive interacts with current string velocity */
        float drive = v->bow_amt * (0.05f + 0.15f * v->vel);
        /* self-limiting: the bow feeds in less as the string gets loud, so a
         * long loop_gain can't integrate the excitation up to a clip. */
        float head = 1.0f - (loop >= 0 ? loop : -loop);
        if (head < 0.0f) head = 0.0f;
        loop += (v->bow_lp * 0.5f + 0.5f * pp_tanhf(loop * 2.0f)) * drive * 0.15f * head;
    }

    /* one-shot hammer energy already sits in the buffer; nothing to add here */

    /* write back into the delay line */
    v->buf[v->widx] = loop;
    v->widx++; if (v->widx >= PP_DELAY_MAX) v->widx = 0;

    float out = d;  /* tap the damped string as the voice output */

    /* send to sympathetic bus (scaled outside by symp_send) */
    *symp_in += out;

    /* voice death test: if it's quiet, un-held and not bowing, retire it */
    if (!v->held && v->loop_gain < 0.999f) {
        float a = out >= 0 ? out : -out;
        static const float FLOOR = 2.0e-4f;
        if (a < FLOOR && v->damp_z > -FLOOR && v->damp_z < FLOOR) {
            v->age = -1; /* mark; culled in render loop after a short hold */
        }
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
    st->p[PP_P_HAMMER]      = 0.4f;
    st->p[PP_P_SYMPATHETIC] = 0.25f;
    st->p[PP_P_DISTURB]     = 0.0f;
    st->p[PP_P_REVERB]      = 0.25f;
    st->polyphony           = PP_MAX_VOICES;
    st->symp_send           = st->p[PP_P_SYMPATHETIC];

    for (int i = 0; i < PP_MAX_VOICES; i++) voice_silence(&st->voices[i]);
    symp_init(st);
    reverb_init(&st->rvb, st->sr);
    return st;
}

void pp_destroy(pp_state_t *st) { if (st) free(st); }

void pp_reset(pp_state_t *st) {
    if (!st) return;
    for (int i = 0; i < PP_MAX_VOICES; i++) voice_silence(&st->voices[i]);
    symp_init(st);
    reverb_init(&st->rvb, st->sr);
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

        /* headroom / gentle bus saturation before reverb, then master trim */
        mix_l = pp_tanhf(mix_l * 0.5f) * 0.85f;
        mix_r = pp_tanhf(mix_r * 0.5f) * 0.85f;

        float l, r;
        reverb_process(&st->rvb, mix_l, mix_r, st->p[PP_P_REVERB], &l, &r);
        out_l[f] = l * 0.9f;
        out_r[f] = r * 0.9f;
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
