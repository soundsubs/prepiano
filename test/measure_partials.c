/*
 * measure_partials.c  -  offline analysis harness for PrePiano strings.
 *
 * Strikes one note, lets it ring, and measures the frequency of each partial
 * with a fine Goertzel scan around the harmonic grid. Reports:
 *   - fundamental tuning error in cents (is the note still in tune?)
 *   - the measured partial-stretch ratio f_k / (k * f0)
 *   - the implied inharmonicity coefficient B per partial and the mean
 *
 * This is the ground truth for the dispersion-filter work: a stiff piano
 * string has f_k = k*f0*sqrt(1 + B*k^2), so ratios climb above 1.0 and B is
 * roughly constant across partials. A compressing filter (the old bug) shows
 * ratios BELOW 1.0.
 *
 *   cc -O3 -o build/measure test/measure_partials.c src/prepiano_dsp.c -lm
 *   ./build/measure <note> <gauge0..1> <felt0..1>
 */
#include "../src/prepiano_dsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SR 44100.0
#define ANALYZE_S 1.4       /* window length analysed                    */
#define SKIP_S    0.20      /* skip the strike transient                 */

static double mtof(int n){ return 440.0 * pow(2.0, (n-69)/12.0); }

/* Goertzel magnitude at frequency f over a Hann-windowed block. */
static double goertzel(const float *x, int N, double f) {
    double w = 2.0 * M_PI * f / SR;
    double cr = cos(w), coeff = 2.0 * cr;
    double s0, s1 = 0.0, s2 = 0.0;
    for (int n = 0; n < N; n++) {
        double win = 0.5 - 0.5 * cos(2.0 * M_PI * n / (N - 1));
        s0 = win * x[n] + coeff * s1 - s2;
        s2 = s1; s1 = s0;
    }
    double re = s1 - s2 * cr;
    double im = s2 * sin(w);
    return sqrt(re*re + im*im);
}

/* Find the magnitude-peak frequency in [flo,fhi] by coarse-then-fine scan. */
static double peak_freq(const float *x, int N, double flo, double fhi) {
    double best_f = flo, best_m = -1.0;
    for (double f = flo; f <= fhi; f += 0.25) {
        double m = goertzel(x, N, f);
        if (m > best_m) { best_m = m; best_f = f; }
    }
    /* refine around the coarse peak */
    double lo = best_f - 0.25, hi = best_f + 0.25;
    for (double f = lo; f <= hi; f += 0.02) {
        double m = goertzel(x, N, f);
        if (m > best_m) { best_m = m; best_f = f; }
    }
    return best_f;
}

int main(int argc, char **argv) {
    int   note  = argc > 1 ? atoi(argv[1]) : 60;
    float gauge = argc > 2 ? (float)atof(argv[2]) : 0.2f;
    float felt  = argc > 3 ? (float)atof(argv[3]) : 0.05f;

    pp_state_t *st = pp_create((float)SR);
    pp_seed(st, 0xA5A5A5u);
    /* isolate the string: no reverb/sympathetic, low felt so partials sing */
    pp_set_param(st, PP_P_FELT, felt);
    pp_set_param(st, PP_P_ATTACK, 0.0f);
    pp_set_param(st, PP_P_DECAY, 0.9f);
    pp_set_param(st, PP_P_GAUGE, gauge);
    pp_set_param(st, PP_P_HAMMER, 0.5f);
    pp_set_param(st, PP_P_SYMPATHETIC, 0.0f);
    pp_set_param(st, PP_P_DISTURB, 0.0f);
    pp_set_param(st, PP_P_REVERB, 0.0f);

    int skip = (int)(SKIP_S * SR);
    int N    = (int)(ANALYZE_S * SR);
    int total = skip + N;
    float *L = malloc(sizeof(float)*total);
    float *R = malloc(sizeof(float)*total);

    pp_note_on(st, note, 110);
    pp_render_float(st, L, R, total);   /* note rings the whole time */

    float *seg = L + skip;              /* analysis window (mono, left) */

    double f_expect = mtof(note);
    /* measure partial 1 first to get the true f0 */
    double f1 = peak_freq(seg, N, f_expect*0.90, f_expect*1.10);
    double cents = 1200.0 * log2(f1 / f_expect);

    printf("note %d  target f0 = %.3f Hz   measured f0 = %.3f Hz   (%+.1f cents)\n",
           note, f_expect, f1, cents);
    printf("  k     f_k(Hz)   ratio f_k/(k*f0)   impliedB\n");

    double bsum = 0.0; int bn = 0;
    for (int k = 1; k <= 12; k++) {
        double center = k * f1;
        double span = 0.02 * center + 3.0;   /* search widens for high k */
        double fk = peak_freq(seg, N, center - span, center + span);
        double ratio = fk / (k * f1);
        double B = (ratio*ratio - 1.0) / (double)(k*k);
        printf("  %2d  %9.3f      %8.5f       %+9.6f\n", k, fk, ratio, B);
        if (k >= 2) { bsum += B; bn++; }
    }
    printf("  mean implied B (partials 2..12) = %.6f\n", bsum / bn);

    free(L); free(R);
    pp_destroy(st);
    return 0;
}
