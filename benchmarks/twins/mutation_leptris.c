#include <leptris.h>
#include <stdio.h>
#include <time.h>
static double now_us(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e6+t.tv_nsec/1e3;}
int main(void){
    double best=1e18;
    for(int r=0;r<400;r++){
        double t0=now_us();
        LeptrisDocument d=leptris_document_create();
        LeptrisElement root=leptris_element_create(d,"r");
        leptris_document_set_root(d,root);
        for(int i=0;i<10000;i++){
            LeptrisElement c=leptris_element_create(d,"n");
            leptris_element_append_child(root,c);
        }
        double t1=now_us();
        leptris_document_free(d);
        if(t1-t0<best)best=t1-t0;
    }
    printf("leptris create+append 10k: %.1f us\n",best);
    best=1e18;
    for(int r=0;r<400;r++){
        LeptrisDocument d=leptris_document_create();
        LeptrisElement root=leptris_element_create(d,"r");
        leptris_document_set_root(d,root);
        LeptrisElement c=leptris_element_create(d,"n");
        leptris_element_append_child(root,c);
        double t0=now_us();
        for(int i=0;i<10000;i++)
            leptris_element_set_attribute(c,"k","v");
        double t1=now_us();
        leptris_document_free(d);
        if(t1-t0<best)best=t1-t0;
    }
    printf("leptris set-attr 10k: %.1f us\n",best);
    return 0;
}
