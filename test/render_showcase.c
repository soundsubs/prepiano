/*
 * render_showcase.c  -  a focused listen at the two new features:
 *   (1) stiff-string INHARMONICITY (stretched partials that stay in tune)
 *   (2) detuned UNISON strings (440/441/439) -> shimmer + double decay
 *
 * Deliberately sparse and slow so each effect is audible.
 *
 *   cc -O3 -o build/showcase test/render_showcase.c src/prepiano_dsp.c -lm
 *   ./build/showcase build/prepiano_showcase.wav
 */
#include "../src/prepiano_dsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define SR 44100.0f

static int16_t *g_buf=NULL; static int g_cap=0,g_len=0;
static void ensure(int e){ if(g_len+e<=g_cap)return; while(g_len+e>g_cap)g_cap=g_cap?g_cap*2:(1<<18); g_buf=realloc(g_buf,(size_t)g_cap*2*sizeof(int16_t)); }
static void render(pp_state_t*st,float s){ int n=(int)(s*SR); ensure(n); pp_render_int16(st,g_buf+g_len*2,n); g_len+=n; }
static void write_wav(const char*p,const int16_t*d,int fr){ FILE*f=fopen(p,"wb"); int ch=2,bits=16,sr=(int)SR,br=sr*ch*bits/8,ba=ch*bits/8,db=fr*ba,riff=36+db; fwrite("RIFF",1,4,f);fwrite(&riff,4,1,f);fwrite("WAVE",1,4,f);fwrite("fmt ",1,4,f); int fl=16;short fm=1;fwrite(&fl,4,1,f);fwrite(&fm,2,1,f);short c=ch;fwrite(&c,2,1,f);fwrite(&sr,4,1,f);fwrite(&br,4,1,f);short b=ba;fwrite(&b,2,1,f);short bp=bits;fwrite(&bp,2,1,f);fwrite("data",1,4,f);fwrite(&db,4,1,f);fwrite(d,1,db,f);fclose(f); }

int main(int argc,char**argv){
    const char*out=argc>1?argv[1]:"prepiano_showcase.wav";
    pp_state_t*st=pp_create(SR); pp_seed(st,0x5150u);
    #define K(i,v) pp_set_param(st,i,(v))
    #define ON(n,vel) pp_note_on(st,(n),(vel))
    #define OFF(n) pp_note_off(st,(n))

    /* voice the model to expose partials: low felt (bright), long decay,
     * a little gauge for stiffness, gentle room. */
    K(PP_P_FELT,0.10f); K(PP_P_ATTACK,0.0f); K(PP_P_DECAY,0.85f);
    K(PP_P_GAUGE,0.30f); K(PP_P_HAMMER,0.42f); K(PP_P_SYMPATHETIC,0.30f);
    K(PP_P_DISTURB,0.0f); K(PP_P_REVERB,0.22f);

    /* 1) single notes up the keyboard: hear the tone stay in tune while the
     *    upper partials stretch (bass warm, treble bell-like). */
    int up[6]={40,52,60,67,76,84};
    for(int i=0;i<6;i++){ ON(up[i],96); render(st,1.6f); OFF(up[i]); render(st,0.15f); }
    render(st,0.6f);

    /* 2) a held mid triad -- the detuned unison strings beat against each
     *    other: listen for the slow shimmer and the long "aftersound" tail. */
    ON(55,88); ON(59,88); ON(62,88);
    render(st,5.0f);
    OFF(55); OFF(59); OFF(62);
    render(st,1.4f);

    /* 3) stretched octaves: play C3 then C4 then C5 together -- a piano's
     *    octaves are tuned slightly wide because of this inharmonicity. */
    ON(48,92); render(st,0.5f);
    ON(60,92); render(st,0.5f);
    ON(72,92); render(st,3.0f);
    OFF(48); OFF(60); OFF(72);
    render(st,1.6f);

    /* 4) a low bass note held long, then a bright treble sparkle. */
    ON(33,104); render(st,3.2f); OFF(33); render(st,0.3f);
    int spk[4]={79,83,86,88};
    for(int i=0;i<4;i++){ ON(spk[i],100); render(st,0.28f); OFF(spk[i]); }
    render(st,2.2f);

    write_wav(out,g_buf,g_len);
    printf("wrote %s  (%.2f s)\n",out,g_len/SR);
    pp_destroy(st); free(g_buf); return 0;
}
