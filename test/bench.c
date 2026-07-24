/*
 * bench.c  -  CPU cost of the PrePiano core. Renders a fixed workload with all
 * voices busy and reports the realtime factor (audio-seconds per wall-second).
 * The Move must clear realtime (>1x) with big headroom; x86 numbers here scale
 * down by roughly 30-50x on the Move's CPU, so aim high.
 *
 *   cc -O3 -o build/bench test/bench.c src/prepiano_dsp.c -lm
 *   ./build/bench <voices>
 */
#include "../src/prepiano_dsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SR 44100.0f

int main(int argc, char **argv) {
    int voices = argc > 1 ? atoi(argv[1]) : 8;
    pp_state_t *st = pp_create(SR);
    pp_set_param(st, PP_P_POLYPHONY, (float)voices);
    pp_set_param(st, PP_P_REVERB, 0.5f);
    pp_set_param(st, PP_P_SYMPATHETIC, 0.4f);
    pp_set_param(st, PP_P_GAUGE, 0.3f);

    /* fill every voice: a fat cluster across the trichord (3-string) region */
    for (int i = 0; i < voices; i++) pp_note_on(st, 55 + i * 2, 100);

    int blocks = 8000;               /* 8000 * 128 / 44100 ~= 23.2 s of audio  */
    int16_t buf[128 * 2];
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int b = 0; b < blocks; b++) {
        pp_render_int16(st, buf, 128);
        if (b == blocks / 2) for (int i = 0; i < voices; i++) pp_note_off(st, 55 + i * 2);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double wall = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    double audio = blocks * 128.0 / SR;
    printf("voices=%2d  audio=%.2fs  wall=%.3fs  realtime=%.1fx  (%.1f%% of one x86 core)\n",
           voices, audio, wall, audio / wall, 100.0 * wall / audio);

    pp_destroy(st);
    return 0;
}
