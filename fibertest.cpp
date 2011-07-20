#include <stdlib.h>
#include <stdio.h>
#include "switch.h"


struct f1_init_data{
  waiter* other;
  waiter* wmain;
};
waiter* inv(waiter* other){
  waiter w;
  return w.invoke<waiter*>(other, &w);
}
#define COUNT 10000000
__attribute__((swapstack,section("elsewhere"))) void f1(func_t* fn, func_t** loc, f1_init_data d){
  announce_paused(loc, fn);
  printf("lolz\n");
  waiter* other = d.other;
  waiter w;
  for (int i=0; i<COUNT/4; i++){
    other = inv(other);
    other = inv(other);
    other = inv(other);
    other = inv(other);
    /*
    { w.func = 0; other = w.invoke<waiter*>(other, &w); }
    { w.func = 0; other = w.invoke<waiter*>(other, &w); }
    { w.func = 0; other = w.invoke<waiter*>(other, &w); }
    { w.func = 0; other = w.invoke<waiter*>(other, &w); }
    */

  }
  waiter w2;
  w2.invoke<void>(d.wmain);
}

__attribute__((swapstack)) void f2(func_t* fn, func_t** loc, waiter* other){
  announce_paused(loc, fn);
  waiter w;
  while (1){
    { w.func = 0; other = w.invoke<waiter*>(other, &w); }
    { w.func = 0; other = w.invoke<waiter*>(other, &w); }
    { w.func = 0; other = w.invoke<waiter*>(other, &w); }
    { w.func = 0; other = w.invoke<waiter*>(other, &w); }

  }
}

static __inline__ unsigned long long rdtsc(void)
{
  unsigned hi, lo;
  __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
  return ( (unsigned long long)lo)|( ((unsigned long long)hi)<<32 );
}



#define SS 1024*1024
int main(){
  void* f1m = __builtin_newstack(malloc(SS), SS, (void*)f1);
  waiter f1init; f1init.func = (func_t*)f1m;

  void* f2m = __builtin_newstack(malloc(SS), SS, (void*)f2);
  waiter f2init; f2init.func = (func_t*)f2m;

  printf("main starting f2\n");
  waiter self1;
  waiter* f2w = self1.invoke<waiter*>(&f2init, &self1);
  
  printf("main starting f1\n");
  waiter self2;
  f1_init_data d = {f2w, &self2};
  unsigned long long before = rdtsc();
  self2.invoke<void>(&f1init, d);
  unsigned long long after = rdtsc();
  printf("M  : cycles: %.2f\n", (double)(after - before) / (double)COUNT / 2);
  printf("main is exiting\n");
}
