/* brightness trajectory: spectral centroid (in units of f0) over time for one
 * note. cc -O3 -o build/bt test/bright_traj.c src/prepiano_dsp.c -lm
 * ./build/bt <note> <felt> <gauge> <hammer> */
#include "../src/prepiano_dsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#define SR 44100.0
static double gz(const float*x,int N,double f){double w=2*M_PI*f/SR,cr=cos(w),co=2*cr,s0,s1=0,s2=0;for(int n=0;n<N;n++){s0=x[n]+co*s1-s2;s2=s1;s1=s0;}double re=s1-s2*cr,im=s2*sin(w);return sqrt(re*re+im*im)/N;}
int main(int c,char**v){
    int note=c>1?atoi(v[1]):57; float ft=c>2?atof(v[2]):0,ga=c>3?atof(v[3]):1,ha=c>4?atof(v[4]):1;
    pp_state_t*s=pp_create(SR); pp_seed(s,0x9u);
    pp_set_param(s,PP_P_FELT,ft);pp_set_param(s,PP_P_ATTACK,0);pp_set_param(s,PP_P_DECAY,1);
    pp_set_param(s,PP_P_GAUGE,ga);pp_set_param(s,PP_P_HAMMER,ha);
    pp_set_param(s,PP_P_SYMPATHETIC,0);pp_set_param(s,PP_P_DISTURB,0);pp_set_param(s,PP_P_REVERB,0);
    int n=(int)(4*SR); float*L=malloc(4*n),*R=malloc(4*n);
    pp_note_on(s,note,120); pp_render_float(s,L,R,n);
    double f0=440*pow(2.0,(note-69)/12.0);
    double ts[6]={0.05,0.2,0.5,1.0,2.0,3.0};
    printf("note %d f0=%.0f centroid(xf0): ",note,f0);
    for(int t=0;t<6;t++){ const float*sg=L+(int)(ts[t]*SR); double nu=0,de=0;
        for(int k=1;k<=20;k++){double fk=k*f0; if(fk>SR*0.45)break; double m=gz(sg,(int)(0.04*SR),fk); nu+=k*m; de+=m;}
        printf("%.1fs=%.2f  ",ts[t], de>1e-9?nu/de:0); }
    printf("\n"); free(L);free(R);pp_destroy(s);return 0;
}
