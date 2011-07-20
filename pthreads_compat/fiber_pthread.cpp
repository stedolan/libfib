#define FIBER_PTHREAD_NO_RENAME

// FIXME include deps
#include <stdio.h>
#include <stdlib.h>
#include <new>
#include <pthread.h>
#include <unistd.h>
#define FIBER_FULL_DEFINITIONS
#include "fiber_pthread.h"
#include "../sync_object.h"
#include "../spawn.h"

static struct worker_spawner{
  worker_spawner(){
#ifndef QUIET
    printf("Spawning workers\n");
#endif
    worker::spawn_workers(atoi(getenv("FIBER_WORKERS")));
#ifndef QUIET
    printf("Workers running\n");
#endif
  }
  ~worker_spawner(){
#ifndef QUIET
    printf("Awaiting workers\n");
#endif
    worker::await_completion();
#ifndef QUIET
    printf("Workers done\n");
#endif
  }
} spawner;

int fiber_pthread_mutex_init(fiber_pthread_mutex_t* mtx, fiber_pthread_mutexattr_t*){
  mtx->sync.init();
  return 0;
}

int fiber_pthread_mutex_lock(fiber_pthread_mutex_t* mtx){
  mutex::lock(mtx->sync);
  return 0;
}

int fiber_pthread_mutex_unlock(fiber_pthread_mutex_t* mtx){
  mutex::unlock(mtx->sync);
  return 0;
}

int fiber_pthread_mutex_destroy(fiber_pthread_mutex_t* mtx){
  //FIXME: assert(unlocked);
  return 0;
}

int fiber_pthread_cond_init(fiber_pthread_cond_t* cond, fiber_pthread_condattr_t* attr){
  cond->sync.init();
  return 0;
}
int fiber_pthread_cond_wait(fiber_pthread_cond_t* cond, fiber_pthread_mutex_t* mtx){
  condition_var::wait(cond->sync, mtx->sync);
  return 0;
}
int fiber_pthread_cond_signal(fiber_pthread_cond_t* cond){
  condition_var::signal(cond->sync);
  return 0;
}
int fiber_pthread_cond_broadcast(fiber_pthread_cond_t* cond){
  condition_var::broadcast(cond->sync);
  return 0;
}
int fiber_pthread_cond_destroy(fiber_pthread_cond_t* cond){
  //FIXME: assert(!blocked);
  return 0;
}

int fiber_pthread_attr_init(fiber_pthread_attr_t* attr){
  attr->stacksize = 0;
  return 0;
}
int fiber_pthread_attr_setscope(fiber_pthread_attr_t* attr){
  // ignored
  return 0;
}
int fiber_pthread_attr_setstacksize(fiber_pthread_attr_t* attr, size_t size){
  attr->stacksize = size;
  return 0;
}

int fiber_pthread_create(fiber_pthread_t* thread, fiber_pthread_attr_t* attr,
                         void* (*start_fn)(void*), void* arg){
  static int first_pthread = 1;
  if (first_pthread){
    for (int i=0; i<worker::nworkers; i++){
      worker::all_workers[i]->stats.reset();
    }
  }
  first_pthread = 0;
  size_t stacksize = attr ? attr->stacksize : 0;
  running_fiber<void*>* fib = spawn_fiber(start_fn, arg, stacksize).fib;
  *thread = (void*)fib;
  return 0;
}

int fiber_pthread_join(fiber_pthread_t thread, void** ret){
  running_fiber<void*>* fib = (running_fiber<void*>*)thread;
  void* ans = fib->blk.accept()->data;
  free(fib->stack);
  if (ret) *ret = ans;
  return 0;
}

int fiber_pthread_yield(){
  worker::current().yield();
  return 0;
}
