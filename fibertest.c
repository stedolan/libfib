#include <stdio.h>
#include <stdlib.h>
#define SWAPSTACK __attribute__((swapstack))


//typedef unsigned long long argtype;
typedef double argtype;

typedef struct{
  void* other;
  int a;
} ret;
typedef SWAPSTACK ret (*ctx_switch)(argtype);

argtype total_f;
int total_i;

#define COUNT 100000000

__attribute__((section("hax"))) SWAPSTACK void a(SWAPSTACK void* (*fib)(void), ctx_switch other){
  //  printf(" A : In a\n");
  argtype f = 42;
  int total_i_local = 0;
  for (int i=0; i<COUNT/8; i++){
    ret ans = other(f);
    //    printf(" A : Got %d from B\n", ans.a);
    //    total_i_local += ans.a;
    //    total_i += ans.a;
    other = (ctx_switch)ans.other;

    ans = other(f);
    other = (ctx_switch)ans.other;
    ans = other(f);
    other = (ctx_switch)ans.other;
    ans = other(f);
    other = (ctx_switch)ans.other;

    ans = other(f);
    other = (ctx_switch)ans.other;
    ans = other(f);
    other = (ctx_switch)ans.other;
    ans = other(f);
    other = (ctx_switch)ans.other;
    ans = other(f);
    other = (ctx_switch)ans.other;


  }
  total_i += total_i_local;
  //  printf(" A : Back in a\n");
  fib();
}

typedef struct {
  SWAPSTACK void (*other)(int);
  argtype a;
}ret2;
typedef SWAPSTACK ret2 (*pauser)(int);
SWAPSTACK void b(pauser fib){
  //  printf("  B: init\n");
  argtype total_f_local = 0;
  while (1){
    ret2 r = fib(44);
    //    total_f += r.a;
    //    printf("  B: resumed, got %f\n", r.a);
    fib = (pauser)r.other;


    ret2 r_ = fib(44);
    fib = (pauser)r_.other;
    r_ = fib(44);
    fib = (pauser)r_.other;
    r_ = fib(44);
    fib = (pauser)r_.other;
    r_ = fib(44);
    fib = (pauser)r_.other;
    r_ = fib(44);
    fib = (pauser)r_.other;
    r_ = fib(44);
    fib = (pauser)r_.other;
    r_ = fib(44);
    fib = (pauser)r_.other;
    r_ = fib(44);
    fib = (pauser)r_.other;
    r_ = fib(44);
    fib = (pauser)r_.other;


  }
  //  total_f = total_f_local;
  //  printf("  B: resumed again\n");
}

#define SS 1024*1024
void* getmem(){
  void* p = malloc(SS + 32);
  unsigned long p_ = (unsigned long)p;
  p_ &= ~32;
  p_ += 32 + 3;
  return (void*)p_;
}
static __inline__ unsigned long long rdtsc(void)
{
  unsigned hi, lo;
  __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
  return ( (unsigned long long)lo)|( ((unsigned long long)hi)<<32 );
}


int main(){
  void* afib_ = __builtin_newstack(getmem(), SS, (void*)a);
  void* bfib_ = __builtin_newstack(getmem(), SS, (void*)b);
  printf("%p\n", afib_);
  SWAPSTACK ret (*bfib)(void) = bfib_;
  printf("M  : Starting b\n");
  ctx_switch b_paused = bfib().other;
  printf("M  : b started, running a\n");
  SWAPSTACK void* (*afib)(ctx_switch) = afib_;
  unsigned long long before = rdtsc();
  afib(b_paused);
  unsigned long long after = rdtsc();
  printf("M  : totals: %d, %llu\n", total_i, (unsigned long long)total_f);
  printf("M  : cycles: %.2f\n", (double)(after - before) / (double)COUNT / 2);
  printf("M  : exiting\n");
}
