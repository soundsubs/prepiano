/*
 * host/plugin_api_v1.h  -  REFERENCE STUB
 *
 * This is a local, self-contained copy of the Schwung host/plugin ABI as
 * documented in charlesvestal/schwung docs/API.md, provided so that
 * prepiano_plugin.c can be compiled and type-checked off-hardware.
 *
 * On a real Move build, the Schwung source tree provides the authoritative
 * header of the same path; put the Schwung `src/` on the include path AHEAD
 * of this one (or delete this file) so the real definitions win. The struct
 * layouts below match the published v2 plugin API and host_api_v1_t.
 */
#ifndef SCHWUNG_PLUGIN_API_V1_H
#define SCHWUNG_PLUGIN_API_V1_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host services handed to every plugin at init. */
typedef struct host_api_v1 {
    uint32_t api_version;
    int      sample_rate;        /* 44100                      */
    int      frames_per_block;   /* 128                        */
    uint8_t *mapped_memory;      /* direct mailbox access      */
    int      audio_out_offset;
    int      audio_in_offset;
    void   (*log)(const char *msg);
    int    (*midi_send_internal)(const uint8_t *msg, int len);
    int    (*midi_send_external)(const uint8_t *msg, int len);
    int    (*get_clock_status)(void);
} host_api_v1_t;

/* Sound-generator plugin, API v2 (multi-instance, Signal-Chain ready). */
typedef struct plugin_api_v2 {
    uint32_t api_version;                                   /* = 2 */
    void*  (*create_instance)(const char *module_dir, const char *json_defaults);
    void   (*destroy_instance)(void *instance);
    void   (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void   (*set_param)(void *instance, const char *key, const char *val);
    int    (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void   (*render_block)(void *instance, int16_t *out_lr, int frames);
} plugin_api_v2_t;

#ifdef __cplusplus
}
#endif
#endif
