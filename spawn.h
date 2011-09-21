#ifndef SPAWN_H
#define SPAWN_H
#include "sched.h"
#include "sync_object.h"
#include "switch.h"



template <class T>
struct running_fiber {
  blocker<T> blk;
  void* stack;
};


typedef unsigned long long stackmem[(102400 + sizeof(running_fiber<void*>))/8];

#if 0
enum {NSTK = 10000};
__thread stackmem* stk;
__thread int maxstk;
__thread stackmem* stknext;
void* alloc_stack(){
  if (!stk){
    stk = (stackmem*)malloc(sizeof(stackmem) * NSTK);
  }
  if (stknext){
    void* mem = (void*)stknext;
    stknext = *(stackmem**)stknext;
    return mem;
  }else{
    assert(maxstk < NSTK);
    return (void*)&stk[maxstk++];
  }
}
void free_stack(void* p){
  *(stackmem**)p = stknext;
  stknext = (stackmem*)p;
}

#endif


template <class T>
struct fiber_handle{
  running_fiber<T>* fib;
  T join(){
    assert(fib);
    T data = fib->blk.accept()->data;
    //free_stack(fib->stack);
    free(fib->stack);
    fib = 0;
    return data;
  }
  void detach(){
    //FIXME ohshit
  }
};

/*
template <class T>
T fiber_handle_join(void* fib){
  fiber_handle fibh;
  fibh.fib = (running_fiber<T>*)fib;
  return fibh.join();
}
*/




template <class Ret, class Arg, Ret (Func)(Arg)>
static SWAPSTACK void fiber_init_thunk_fixed(func_t* fib, func_t** loc, 
                                       Arg arg, 
                                       blocker<Ret>* comp){
  announce_paused(loc, fib);
  
  Ret ret = Func(arg);
  say("Spawned fiber terminating\n");
  comp->block(ret);
}


// FIXME alignment
template <class Ret, class Arg, Ret (Func)(Arg)>
fiber_handle<Ret> spawn_fiber_fixed(Arg arg, size_t stacksize = 0){
    if (!stacksize) stacksize = STACKSIZE;
  //  void* stack = malloc(stacksize + sizeof(running_fiber<T>));
  //  running_fiber<T>* rfib = new ((char*)stack + stacksize) running_fiber<T>();
  //stacksize = sizeof(stackmem);
  //void* stack = alloc_stack();
  void* stack = malloc(stacksize);
  running_fiber<Ret>* rfib = new ((char*)stack + stacksize - sizeof(running_fiber<Ret>)) running_fiber<Ret>();
  stacksize -= sizeof(running_fiber<Ret>);

  rfib->stack = stack;
  typedef SWAPSTACK typename switcher<void,void>::retval 
    (*new_fiber_t)(func_t**, Arg, blocker<Ret>*);
  new_fiber_t fiber = (new_fiber_t)__builtin_newstack(stack, stacksize, (void*)fiber_init_thunk_fixed<Ret,Arg,Func>);
  say("spawning\n");
  waiter<void> w;
  worker::current().push_runqueue(&w);
  switcher<void,void>::retval rv = fiber(&w.func, arg, &rfib->blk);
  announce_paused(rv.store_loc, rv.function);
  
  fiber_handle<Ret> fibh;
  fibh.fib = rfib;
  return fibh;
}


static blocker<void*> detached_blocker;

template <class Arg, void (Func)(Arg)>
static SWAPSTACK void fiber_init_thunk_detach(func_t* fib, func_t** loc, 
                                              Arg arg, 
                                              void* stk){
  announce_paused(loc, fib);
  Func(arg);
  say("fiber dying\n");
  detached_blocker.block(stk);
  say("fiber dead?!?\n");
}

template <class Arg, void (Func)(Arg)>
void spawn_fiber_fixed_detached(Arg arg, size_t stacksize, void* stack){
  typedef SWAPSTACK typename switcher<void,void>::retval 
    (*new_fiber_t)(func_t**, Arg, void*);
  say("spawning\n");
  waiter<void> w;
  worker::current().push_runqueue(&w);
  new_fiber_t fiber = (new_fiber_t)__builtin_newstack(stack, stacksize, (void*)fiber_init_thunk_detach<Arg,Func>);
  switcher<void,void>::retval rv = fiber(&w.func, arg, stack);
  announce_paused(rv.store_loc, rv.function);
}







template <class Ret, class Arg, Ret (Func)(Arg)>
struct task{
  fiber_handle<Ret> fib;
  task(Arg arg) : fib(spawn_fiber_fixed<Ret,Arg,Func>(arg, 1024)){
  }
  Ret join(){
    return fib.join();
  }
};




template <class Ret, class Arg>
static SWAPSTACK void fiber_init_thunk2(func_t* fib, func_t** loc, 
                                       Ret (*func)(Arg), Arg arg, 
                                       blocker<Ret>* comp){
  announce_paused(loc, fib);
  
  Ret ret = func(arg);
  say("Spawned fiber terminating\n");
  comp->block(ret);
}

// FIXME alignment
template <class Ret, class Arg>
fiber_handle<Ret> spawn_fiber(Ret (*func)(Arg), Arg arg, size_t stacksize = 0){
  //  if (!stacksize) stacksize = STACKSIZE;
  //  void* stack = malloc(stacksize + sizeof(running_fiber<T>));
  //  running_fiber<T>* rfib = new ((char*)stack + stacksize) running_fiber<T>();
  stacksize = sizeof(stackmem);
  //void* stack = alloc_stack();
  void* stack = malloc(stacksize);
  running_fiber<Ret>* rfib = new ((char*)stack + stacksize - sizeof(running_fiber<Ret>)) running_fiber<Ret>();
  stacksize -= sizeof(running_fiber<Ret>);

  rfib->stack = stack;
  typedef SWAPSTACK typename switcher<void,void>::retval 
    (*new_fiber_t)(func_t**, Ret (*)(Arg), Arg, blocker<Ret>*);
  new_fiber_t fiber = (new_fiber_t)__builtin_newstack(stack, stacksize, (void*)fiber_init_thunk2<Ret,Arg>);
  say("spawning\n");
  waiter<void> w;
  worker::current().push_runqueue(&w);
  switcher<void,void>::retval rv = fiber(&w.func, func, arg, &rfib->blk);
  announce_paused(rv.store_loc, rv.function);
  
  assert(rfib);
  fiber_handle<Ret> fibh;
  fibh.fib = rfib;
  return fibh;
}


#endif
