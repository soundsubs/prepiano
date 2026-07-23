/*
 * prepiano_plugin.c  -  Schwung sound-generator wrapper for PrePiano.
 *
 * Implements the Schwung DSP Plugin API v2 (see charlesvestal/schwung
 * docs/API.md) on top of the portable core in prepiano_dsp.c. The host
 * calls move_plugin_init_v2() once, then create_instance() per slot.
 *
 * Parameter keys (must match module.json chain_params / ui_hierarchy):
 *   felt attack decay gauge hammer symp disturb reverb   -> 0..1 floats
 *   poly                                                 -> 1..16 int
 *   rndmz                                                -> action (any set)
 *
 * Physical knobs 1..8 in Shadow UI arrive here as set_param() calls; note
 * events arrive via on_midi().
 */
#include "host/plugin_api_v1.h"
#include "prepiano_dsp.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const host_api_v1_t *g_host = NULL;

typedef struct {
    pp_state_t *dsp;
    int   sustain;                 /* CC64 pedal state                    */
    uint8_t held[128];             /* notes physically down (for pedal)   */
} pp_instance_t;

/* map a parameter key string to our enum; returns -1 if unknown */
static int key_to_id(const char *key) {
    if (!key) return -1;
    if (!strcmp(key, "felt"))    return PP_P_FELT;
    if (!strcmp(key, "attack"))  return PP_P_ATTACK;
    if (!strcmp(key, "decay"))   return PP_P_DECAY;
    if (!strcmp(key, "gauge"))   return PP_P_GAUGE;
    if (!strcmp(key, "hammer"))  return PP_P_HAMMER;
    if (!strcmp(key, "symp"))    return PP_P_SYMPATHETIC;
    if (!strcmp(key, "disturb")) return PP_P_DISTURB;
    if (!strcmp(key, "reverb"))  return PP_P_REVERB;
    if (!strcmp(key, "poly"))    return PP_P_POLYPHONY;
    if (!strcmp(key, "rndmz"))   return PP_P_RNDMZ;
    return -1;
}

static void *plug_create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir; (void)json_defaults;
    pp_instance_t *in = (pp_instance_t *)calloc(1, sizeof(pp_instance_t));
    if (!in) return NULL;
    float sr = g_host ? (float)g_host->sample_rate : 44100.0f;
    in->dsp = pp_create(sr);
    if (!in->dsp) { free(in); return NULL; }
    return in;
}

static void plug_destroy_instance(void *instance) {
    pp_instance_t *in = (pp_instance_t *)instance;
    if (!in) return;
    pp_destroy(in->dsp);
    free(in);
}

static void plug_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    pp_instance_t *in = (pp_instance_t *)instance;
    if (!in || len < 1) return;
    uint8_t status = msg[0] & 0xF0u;
    uint8_t d1 = len > 1 ? (msg[1] & 0x7Fu) : 0;
    uint8_t d2 = len > 2 ? (msg[2] & 0x7Fu) : 0;

    if (status == 0x90 && d2 > 0) {                 /* note on */
        in->held[d1] = 1;
        pp_note_on(in->dsp, d1, d2);
    } else if (status == 0x80 || (status == 0x90 && d2 == 0)) { /* note off */
        in->held[d1] = 0;
        if (!in->sustain) pp_note_off(in->dsp, d1);
    } else if (status == 0xB0) {                     /* control change */
        if (d1 == 64) {                              /* sustain pedal */
            in->sustain = d2 >= 64;
            if (!in->sustain) {                      /* release un-held notes */
                for (int n = 0; n < 128; n++)
                    if (!in->held[n]) pp_note_off(in->dsp, n);
            }
        } else if (d1 == 123 || d1 == 120) {         /* all notes / sound off */
            pp_all_notes_off(in->dsp);
            memset(in->held, 0, sizeof(in->held));
        }
    }
}

static void plug_set_param(void *instance, const char *key, const char *val) {
    pp_instance_t *in = (pp_instance_t *)instance;
    if (!in) return;
    int id = key_to_id(key);
    if (id < 0) return;
    float v = val ? (float)atof(val) : 0.0f;
    pp_set_param(in->dsp, (pp_param_t)id, v);
}

static int plug_get_param(void *instance, const char *key, char *buf, int buf_len) {
    pp_instance_t *in = (pp_instance_t *)instance;
    if (!in || !buf || buf_len < 1) return -1;
    int id = key_to_id(key);
    if (id < 0 || id == PP_P_RNDMZ) return -1;
    float v = pp_get_param(in->dsp, (pp_param_t)id);
    int nprint = (id == PP_P_POLYPHONY)
               ? snprintf(buf, buf_len, "%d", (int)(v + 0.5f))
               : snprintf(buf, buf_len, "%.4f", v);
    return (nprint > 0 && nprint < buf_len) ? nprint : -1;
}

static void plug_render_block(void *instance, int16_t *out_lr, int frames) {
    pp_instance_t *in = (pp_instance_t *)instance;
    if (!in) { if (out_lr) memset(out_lr, 0, sizeof(int16_t) * 2 * frames); return; }
    pp_render_int16(in->dsp, out_lr, frames);
}

static plugin_api_v2_t g_api = {
    .api_version     = 2,
    .create_instance = plug_create_instance,
    .destroy_instance= plug_destroy_instance,
    .on_midi         = plug_on_midi,
    .set_param       = plug_set_param,
    .get_param       = plug_get_param,
    .render_block    = plug_render_block,
};

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}
