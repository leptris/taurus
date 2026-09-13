#include <leptris.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now_us(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e6+t.tv_nsec/1e3;}
static void gen(char** out,size_t* n,int attrs,int text){
    size_t cap=1<<22;char*b=malloc(cap);size_t j=0;
    j+=snprintf(b+j,cap-j,"<r>");
    for(int e=0;e<100;e++){
        j+=snprintf(b+j,cap-j,"<e");
        for(int a=0;a<attrs;a++)j+=snprintf(b+j,cap-j," a%d=\"v%d\"",a,a);
        j+=snprintf(b+j,cap-j,">");
        for(int t=0;t<text;t++)j+=snprintf(b+j,cap-j,"<t>hello %d</t>",t);
        j+=snprintf(b+j,cap-j,"</e>");
    }
    j+=snprintf(b+j,cap-j,"</r>");*out=b;*n=j;
}
static double bench(const char*xml,size_t n,int reps){
    double best=1e18;
    for(int r=0;r<reps;r++){
        double t0=now_us();LeptrisStatus st=LEPTRIS_OK;
        LeptrisDocument d=leptris_parse_string(xml,n,&st);
        double t1=now_us();
        if(d)leptris_document_free(d);
        if(t1-t0<best)best=t1-t0;
    }
    return best;
}
int main(void){
    char*x;size_t n;
    gen(&x,&n,50,0);printf("attr-heavy-5k: %.1f us\n",bench(x,n,300));
    gen(&x,&n,0,10);printf("text-heavy-1k: %.1f us\n",bench(x,n,300));
    return 0;
}
