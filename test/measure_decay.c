/*
 * measure_decay.c  -  how DECAY and BRIGHTNESS scale with register.
 * Strikes a note and tracks, over time (10 ms hops):
 *   - the FUNDAMENTAL amplitude (Goertzel at f0)   -> fundamental T60
 *   - total RMS                                     -> perceived-energy T60
 *   - HF/total energy ratio                         -> brightness half-life
 *
 *   cc -O3 -o build/decay test/measure_decay.c src/prepiano_dsp.c -lm
 *   ./build/decay <note> <felt> <gauge> <hammer>
 */
#include "../src/prepiano_dsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define SR 44100.0

static double goertzel(const float *x, int N, double f) {
    double w = 2.0*M_PI*f/SR, cr = cos(w), coeff = 2.0*cr, s0, s1=0, s2=0;
    for (int n=0;n<N;n++){ s0 = x[n] + coeff*s1 - s2; s2=s1; s1=s0; }
    double re=s1-s2*cr, im=s2*sin(w); return sqrt(re*re+im*im)/N;
}

int main(int argc, char **argv) {
    int   note   = argc>1?atoi(argv[1]):60;
    float felt   = argc>2?(float)atof(argv[2]):0.0f;
    float gauge  = argc>3?(float)atof(argv[3]):1.0f;
    float hammer = argc>4?(float)atof(argv[4]):1.0f;

    pp_state_t *st = pp_create((float)SR);
    pp_seed(st, 0x1234u);
    pp_set_param(st, PP_P_FELT, felt);   pp_set_param(st, PP_P_ATTACK, 0.0f);
    pp_set_param(st, PP_P_DECAY, 1.0f);  pp_set_param(st, PP_P_GAUGE, gauge);
    pp_set_param(st, PP_P_HAMMER, hammer);
    pp_set_param(st, PP_P_SYMPATHETIC, 0.0f);
    pp_set_param(st, PP_P_DISTURB, 0.0f); pp_set_param(st, PP_P_REVERB, 0.0f);

    int secs=30, n=(int)(secs*SR);
    float *L=malloc(sizeof(float)*n), *R=malloc(sizeof(float)*n);
    pp_note_on(st, note, 120);
    pp_render_float(st, L, R, n);

    double f0 = 440.0*pow(2.0,(note-69)/12.0);
    int hop=(int)(0.020*SR), nf=n/hop, win=hop;   /* 20 ms frames */
    double pk_fund=0, pk_rms=0;
    double *fund=malloc(sizeof(double)*nf), *rms=malloc(sizeof(double)*nf), *hi=malloc(sizeof(double)*nf);
    for (int f=0; f<nf; f++) {
        const float *seg = L + f*hop;
        fund[f] = goertzel(seg, win, f0);
        double s=0; for(int i=0;i<win;i++) s+=(double)seg[i]*seg[i];
        rms[f]=sqrt(s/win);
        /* HF energy: sum of Goertzel at partials 4..8 (brightness) */
        double h=0; for(int k=4;k<=8;k++){ double fk=k*f0; if(fk<SR*0.45) h+=goertzel(seg,win,fk); }
        hi[f]=h;
        if(fund[f]>pk_fund)pk_fund=fund[f];
        if(rms[f]>pk_rms)pk_rms=rms[f];
    }
    /* spectral centroid (in units of f0: 1=pure fundamental, higher=brighter)
     * sampled at 0.3 s and 1.0 s after onset -- a robust "how bright" gauge. */
    double cent[2]; double when[2]={0.3,1.0};
    for (int s=0;s<2;s++){
        const float *seg = L + (int)(when[s]*SR);
        double num=0, den=0;
        for(int k=1;k<=16;k++){ double fk=k*f0; if(fk>SR*0.45)break;
            double m=goertzel(seg,(int)(0.040*SR),fk); num+=k*m; den+=m; }
        cent[s]= den>1e-9? num/den : 0;
    }

    int start=3;
    double fl_f=pk_fund*pow(10,-60.0/20.0), fl_r=pk_rms*pow(10,-60.0/20.0);
    double t60f=-1,t60r=-1,tb=-1;
    for(int f=start;f<nf;f++) if(fund[f]<=fl_f){t60f=f*0.020;break;}
    for(int f=start;f<nf;f++) if(rms[f]<=fl_r){t60r=f*0.020;break;}
    double b0=hi[start]/(fund[start]+1e-12);
    for(int f=start;f<nf;f++){ double b=hi[f]/(fund[f]+1e-12); if(b<=0.5*b0){tb=(f-start)*0.020;break;} }

    (void)tb;
    printf("note %3d f0=%7.1f | fund_T60=%6s | rms_T60=%6s | centroid@0.3s=%.2f @1.0s=%.2f (xf0)\n",
        note, f0,
        t60f<0?" >30s":({static char b[12];snprintf(b,12,"%5.2fs",t60f);b;}),
        t60r<0?" >30s":({static char b[12];snprintf(b,12,"%5.2fs",t60r);b;}),
        cent[0], cent[1]);
    free(L);free(R);free(fund);free(rms);free(hi); pp_destroy(st); return 0;
}
