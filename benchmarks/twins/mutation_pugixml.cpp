#include <pugixml.hpp>
#include <cstdio>
#include <ctime>
static double now_us(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e6+t.tv_nsec/1e3;}
int main(){
    double best=1e18;
    for(int r=0;r<400;r++){
        double t0=now_us();
        pugi::xml_document d;
        pugi::xml_node root=d.append_child("r");
        for(int i=0;i<10000;i++)root.append_child("n");
        double t1=now_us();
        if(t1-t0<best)best=t1-t0;
    }
    printf("pugi create+append 10k: %.1f us\n",best);
    best=1e18;
    for(int r=0;r<400;r++){
        pugi::xml_document d;
        pugi::xml_node root=d.append_child("r");
        pugi::xml_node c=root.append_child("n");
        double t0=now_us();
        for(int i=0;i<10000;i++)c.append_attribute("k").set_value("v");
        double t1=now_us();
        if(t1-t0<best)best=t1-t0;
    }
    printf("pugi set-attr 10k: %.1f us\n",best);
    return 0;
}
