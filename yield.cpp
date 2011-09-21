#include <stdio.h>
#include <stdlib.h>

#define swapstack __attribute__((swapstack))


typedef swapstack void* (*yieldfn)(int);

swapstack void even_generator(yieldfn yield){
  for (int i = 0; i < 10; i++){
    yield = (yieldfn)yield(42 + i*2);
  }

}

void f(){
  typedef struct { void* cont; int val; } ret;
  typedef swapstack ret (*genfn)();
  genfn gen = (genfn)__builtin_newstack(malloc(4192), 4192, (void*)&even_generator);
  for (int i = 0; i < 10; i++){
    ret r = gen();
    gen = (genfn)r.cont;
    
    printf("Got %d\n", r.val);
  }
}




template <class T>
struct generator{
  typedef void (*genfn)(generator<T>*);
  struct yield_ret{
    void* cont;
    generator<T>* fnptr;
  };
  typedef swapstack yield_ret (*yieldfn)(int, T);
  struct resume_ret{
    void* cont;
    int keepgoing;
    T data;
  };
  typedef swapstack resume_ret (*resfn)(generator<T>*);
  void* cont;
  genfn fnptr;
  enum { STACKSIZE = 4096 };
  char stk[STACKSIZE];
  static swapstack void thunk(void* cont, generator<T>* g){
    g->cont = cont;
    g->fnptr(g);

  }
  generator(genfn g) : fnptr(g){
    cont = __builtin_newstack(&stk[0], STACKSIZE, (void*)thunk);
  }
  void yield(T x){
    yield_ret r = ((yieldfn)cont)(1, x);
    cont = r.cont;
  }
  T next(bool success){
    resume_ret r = ((resfn)cont)(this);
    cont = r.cont;
    return r.data;
  }
};


void even_generator2(generator<int>* g){
  for (int i = 0; i < 10; i++){
    g->yield(42 + i*2);
  }
}
void f2(){
  generator<int> g(even_generator2);
  for (int i = 0; i < 10; i++){
    printf("Got %d\n", g.next());
  }
}




int main(){
  f2();
}
/*
template over types
indicate completion
pass values to yielder
 */
