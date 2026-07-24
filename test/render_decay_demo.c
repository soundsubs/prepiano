/*
 * render_decay_demo.c  -  A/B the register decay fix using the user's patch
 * (felt 0, attack 0, decay 100%, gauge 100%, hammer 100%). Plays sustained
 * single notes A1..A6 so you can hear each note's ring time, then a pedaled
 * climb where the whole chord should sustain evenly instead of the top notes
 * vanishing first.
 *
 *   cc -O3 -o build/decaydemo test/render_decay_demo.c src/prepiano_dsp.c -lm
 *   ./build/decaydemo build/prepiano_decay_demo.wav
 */
#include "../src/prepiano_dsp.h"
#include <stdio.h>
#include <stdlib.h>

#define SR 44100.0f
static int16_t *g=NULL; static int cap=0,len=0;
static void ens(int e){ if(len+e<=cap)return; while(len+e>cap)cap=cap?cap*2:(1<<18); g=realloc(g,(size_t)cap*2*sizeof(int16_t)); }
static void rnd(pp_state_t*s,float sec){ int n=(int)(sec*SR); ens(n); pp_render_int16(s,g+len*2,n); len+=n; }
static void wav(const char*p){ FILE*f=fopen(p,"wb"); int ch=2,bits=16,sr=(int)SR,br=sr*ch*bits/8,ba=ch*bits/8,db=len*ba,rf=36+db; fwrite("RIFF",1,4,f);fwrite(&rf,4,1,f);fwrite("WAVE",1,4,f);fwrite("fmt ",1,4,f);int fl=16;short fm=1;fwrite(&fl,4,1,f);fwrite(&fm,2,1,f);short c=ch;fwrite(&c,2,1,f);fwrite(&sr,4,1,f);fwrite(&br,4,1,f);short b=ba;fwrite(&b,2,1,f);short bp=bits;fwrite(&bp,2,1,f);fwrite("data",1,4,f);fwrite(&db,4,1,f);fwrite(g,1,db,f);fclose(f); }

int main(int argc,char**argv){
    const char*out=argc>1?argv[1]:"prepiano_decay_demo.wav";
    pp_state_t*s=pp_create(SR); pp_seed(s,0x2222u);
    #define K(i,v) pp_set_param(s,i,(v))
    /* the user's patch */
    K(PP_P_FELT,0.0f); K(PP_P_ATTACK,0.0f); K(PP_P_DECAY,1.0f);
    K(PP_P_GAUGE,1.0f); K(PP_P_HAMMER,1.0f); K(PP_P_SYMPATHETIC,0.25f);
    K(PP_P_DISTURB,0.0f); K(PP_P_REVERB,0.20f);

    /* 1) sustained single notes, each held then released: A1..A6 */
    int notes[6]={33,45,57,69,81,93};
    for(int i=0;i<6;i++){ pp_note_on(s,notes[i],112); rnd(s,3.2f); pp_note_off(s,notes[i]); rnd(s,0.25f); }
    rnd(s,0.5f);

    /* 2) a climbing chord, all held (pedal): the top notes should sustain with
     *    the rest instead of dropping out immediately. */
    int chord[5]={45,52,57,64,69};
    for(int i=0;i<5;i++){ pp_note_on(s,chord[i],104); rnd(s,0.28f); }
    rnd(s,5.0f);
    for(int i=0;i<5;i++) pp_note_off(s,chord[i]);
    rnd(s,1.6f);

    wav(out); printf("wrote %s (%.1fs)\n",out,len/SR);
    pp_destroy(s); free(g); return 0;
}
