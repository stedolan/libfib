#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <new>
#include "sched.h"


struct worker_init{
  worker* this_;
  fiber_func* mainfiber;
};


static waiter<void>* terminating_main_thread;

static void* worker_threadfunc(void* worker_init_){
  worker_init* wi = (worker_init*)worker_init_;
  assert(current_worker_ == 0);
  current_worker_ = wi->this_;
  say("  running\n");
  if (wi->mainfiber){
    waiter<void> w;
    w.next = &worker::sentinel;
    worker::current().local_reserved = &w;
    worker::current().new_fiber(wi->mainfiber, 0, &w.func);
  }
  worker::current().scheduler_loop();
  say("  worker terminating\n");
  current_worker_ = 0;
  delete wi;
  return NULL;
}



void worker::spawn_and_join_workers(int nworkers, fiber_func* mainfiber){
  assert(nworkers > 0);
  assert(all_workers == 0);
  worker::nworkers = nworkers;
  all_workers = new worker*[nworkers];
  // set them all active before spawning any
  // this prevents them noticing that there is no work to do and dying
  for (int i=0; i<nworkers; i++){
    void* mem = malloc(sizeof(worker) + 128);
    all_workers[i] = new (mem) worker;
    all_workers[i]->set_active();
  }
  for (int i=0; i<nworkers; i++){
    all_workers[i]->id = i;
    worker_init* wi = new worker_init;
    wi->this_ = all_workers[i];
    wi->mainfiber = i == nworkers-1 ? mainfiber : 0;
    pthread_create(&all_workers[i]->thisthread, 0, 
                   worker_threadfunc, (void*)wi);
  }
  for (int i=0; i<nworkers; i++){
    pthread_join(all_workers[i]->thisthread, 0);
  }
  worker::worker_stats::print_header();
  for (int i=0; i<nworkers; i++){
    all_workers[i]->stats.print_stats(i);
    all_workers[i]->~worker();
    free(all_workers[i]);
  }
  delete[] all_workers;
  all_workers = 0;
}

static __attribute__((swapstack)) void worker_sched_thunk(func_t* fib, worker* thisworker, waiter<void>* mainfib){
  announce_paused(&mainfib->func, fib);
  assert(current_worker_ == thisworker);
  say("  sched starting\n");
  
  // We add ourselves to the local reserved runqueue
  waiter<void> w;
  w.next = &worker::sentinel;
  worker::current().local_reserved = &w;
  worker::current().lifo_push_count = 0;
  // Then switch back to mainfib
  w.invoke(mainfib);
  // This will run as soon as mainfib yields
  worker::current().scheduler_loop();
  say("  working terminating!?\n");
  current_worker_ = 0;
  
  assert(terminating_main_thread);
  waiter<void> dead;
  dead.invoke(terminating_main_thread);
}

void worker::spawn_workers(int nworkers){
  assert(nworkers > 0);
  assert(all_workers == 0);
  worker::nworkers = nworkers;
  all_workers = new worker*[nworkers];
  // Create n workers (the current thread will be the 0'th worker.
  for (int i=0; i<nworkers; i++){
    void* mem = malloc(sizeof(worker) + 128);
    all_workers[i] = new (mem) worker;
    all_workers[i]->set_active();
  }
  // Spawn threads for n-1 workers
  for (int i=1; i<nworkers; i++){
    all_workers[i]->id = i;
    worker_init* wi = new worker_init;
    wi->this_ = all_workers[i];
    wi->mainfiber = 0;
    pthread_create(&all_workers[i]->thisthread, 0, worker_threadfunc, (void*)wi);
  }
  // The current thread becomes a worker
  // We make a fiber to run the scheduler loop
  current_worker_ = all_workers[0];
  void* stack = malloc(102400); //FIXME size
  typedef __attribute__((swapstack)) switcher<void,void>::retval 
    (*schedfiber)(worker*, waiter<void>*);
  schedfiber sched = (schedfiber)__builtin_newstack(stack, 102400, (void*)worker_sched_thunk);
  say("  Spawning sched fiber\n");
  waiter<void> w;
  switcher<void,void>::retval rv = sched(all_workers[0], &w);
  announce_paused(rv.store_loc, rv.function);
  // Now there's a scheduler on the runqueue, so it's safe to yield from this fiber
}



void worker::await_completion(){
  say("main thread terminating\n");
  waiter<void> w;
  assert(!terminating_main_thread);
  terminating_main_thread = &w;
  worker::current().sleep(w);
  // we'll be woken up when the scheduler dies
  assert(all_workers);
  assert(!current_worker_);
  for (int i=1; i<nworkers;i++){
    pthread_join(all_workers[i]->thisthread, 0);
  }
  worker::worker_stats::print_header();
  for (int i=0; i<nworkers; i++){
    all_workers[i]->stats.print_stats(i);
    all_workers[i]->~worker();
    free(all_workers[i]);
  }
  delete[] all_workers;
  all_workers = 0;
}


int worker::nworkers;
worker** worker::all_workers;
llq::word worker::active_workers = 0;
llq::node worker::sentinel;
__thread worker* current_worker_;
