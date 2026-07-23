/*
 * render_demo.c  -  desktop test harness for the PrePiano DSP core.
 *
 * Renders a short tour of the instrument to a 16-bit stereo WAV so the sound
 * can be auditioned off-hardware, exactly like drmach's drmach_demo.wav flow.
 *
 *   cc -O3 -o build/prepiano_demo test/render_demo.c src/prepiano_dsp.c -lm
 *   ./build/prepiano_demo build/prepiano_demo.wav
 */
#include "../src/prepiano_dsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SR 44100.0f

/* ---- tiny WAV writer (16-bit PCM stereo) ---- */
static void write_wav(const char *path, const int16_t *interleaved, int frames) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    int ch = 2, bits = 16, sr = (int)SR;
    int byte_rate = sr * ch * bits / 8;
    int block_align = ch * bits / 8;
    int data_bytes = frames * block_align;
    int riff = 36 + data_bytes;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    int fmt_len = 16; short fmt = 1;
    fwrite(&fmt_len, 4, 1, f); fwrite(&fmt, 2, 1, f);
    short chs = ch; fwrite(&chs, 2, 1, f);
    fwrite(&sr, 4, 1, f); fwrite(&byte_rate, 4, 1, f);
    short ba = block_align; fwrite(&ba, 2, 1, f);
    short bps = bits; fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_bytes, 4, 1, f);
    fwrite(interleaved, 1, data_bytes, f);
    fclose(f);
}

/* growable output buffer */
static int16_t *g_buf = NULL;
static int g_cap = 0, g_len = 0;   /* in frames */
static void ensure(int extra) {
    if (g_len + extra <= g_cap) return;
    while (g_len + extra > g_cap) g_cap = g_cap ? g_cap * 2 : (1 << 16);
    g_buf = (int16_t *)realloc(g_buf, (size_t)g_cap * 2 * sizeof(int16_t));
}
static void render(pp_state_t *st, float seconds) {
    int n = (int)(seconds * SR);
    ensure(n);
    pp_render_int16(st, g_buf + g_len * 2, n);
    g_len += n;
}

static void section(const char *name) { printf("  %s\n", name); }

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "prepiano_demo.wav";
    pp_state_t *st = pp_create(SR);
    pp_seed(st, 0xBEEF01u);

    printf("PrePiano demo tour:\n");

    /* helper macros */
    #define K(id,v)   pp_set_param(st, id, (v))
    #define ON(n,vel) pp_note_on(st, (n), (vel))
    #define OFF(n)    pp_note_off(st, (n))

    /* 1) plain felt piano - a little C-major figure */
    section("1. default felt piano (C-E-G-C arpeggio + chord)");
    K(PP_P_FELT,0.20f); K(PP_P_ATTACK,0.0f); K(PP_P_DECAY,0.55f);
    K(PP_P_GAUGE,0.15f); K(PP_P_HAMMER,0.45f); K(PP_P_SYMPATHETIC,0.30f);
    K(PP_P_DISTURB,0.0f); K(PP_P_REVERB,0.22f);
    int arp[4] = {60,64,67,72};
    for (int i=0;i<4;i++){ ON(arp[i],100); render(st,0.32f); }
    for (int i=0;i<4;i++) OFF(arp[i]);
    ON(48,90);ON(55,90);ON(60,90);ON(64,90); render(st,1.6f);
    OFF(48);OFF(55);OFF(60);OFF(64); render(st,0.8f);

    /* 2) felt up: muted, dampened */
    section("2. FELT high -> muted, short");
    K(PP_P_FELT,0.85f);
    for (int i=0;i<4;i++){ ON(arp[i],110); render(st,0.28f); OFF(arp[i]); }
    render(st,0.5f);

    /* 3) attack -> bowed */
    section("3. ATTACK high -> bowed / sustaining");
    K(PP_P_FELT,0.15f); K(PP_P_ATTACK,0.9f); K(PP_P_DECAY,0.7f);
    ON(62,100); render(st,1.4f); OFF(62); render(st,0.7f);
    ON(57,100);ON(64,100); render(st,1.4f); OFF(57);OFF(64); render(st,0.7f);

    /* 4) heavy gauge + metal hammer -> big, inharmonic, bell-like */
    section("4. GAUGE + HAMMER high -> heavy strings, metal striker");
    K(PP_P_ATTACK,0.0f); K(PP_P_GAUGE,0.9f); K(PP_P_HAMMER,1.0f); K(PP_P_DECAY,0.75f);
    for (int i=0;i<4;i++){ ON(arp[i]-12,110); render(st,0.4f); }
    for (int i=0;i<4;i++) OFF(arp[i]-12);
    render(st,1.5f);

    /* 5) prepared! disturb high -> knives/forks/spoons rattle */
    section("5. DISTURB high -> prepared piano (rattle/buzz)");
    K(PP_P_GAUGE,0.25f); K(PP_P_HAMMER,0.5f); K(PP_P_DISTURB,0.85f); K(PP_P_DECAY,0.6f);
    int mel[6]={60,62,64,65,67,69};
    for (int i=0;i<6;i++){ ON(mel[i],105); render(st,0.35f); OFF(mel[i]); }
    render(st,0.8f);

    /* 6) sympathetic + big wet reverb, far away */
    section("6. SYMPATHETIC + REVERB high -> resonant, distant room");
    K(PP_P_DISTURB,0.1f); K(PP_P_SYMPATHETIC,0.9f); K(PP_P_REVERB,0.9f); K(PP_P_DECAY,0.8f);
    ON(52,80);ON(59,80);ON(64,80); render(st,2.2f);
    OFF(52);OFF(59);OFF(64); render(st,2.5f);

    /* 7) mono + RNDMZ surprise patch */
    section("7. POLYPHONY=1 (mono) + RNDMZ patch");
    K(PP_P_POLYPHONY,1.0f); K(PP_P_REVERB,0.3f); K(PP_P_SYMPATHETIC,0.3f);
    pp_set_param(st, PP_P_RNDMZ, 1.0f);
    for (int i=0;i<6;i++){ ON(mel[i],110); render(st,0.3f); OFF(mel[i]); }
    render(st,1.0f);

    write_wav(out, g_buf, g_len);
    printf("wrote %s  (%.2f s, %d frames)\n", out, g_len / SR, g_len);
    pp_destroy(st);
    free(g_buf);
    return 0;
}
