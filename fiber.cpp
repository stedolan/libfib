#include <stdio.h>
#include <queue>
#include <pthread.h>
#include <cassert>
#include <cstdlib>
using namespace std;

#define SWAPSTACK __attribute__((swapstack))

typedef void (func_t)(void);


#define say(fmt, ...) fprintf(stdout, "WRK%d: " fmt, (int)current_worker->id, ##__VA_ARGS__); //fflush(stdout);

//#define say(fmt, ...) 

struct fiber_queue{
  vector<func_t*> q;
  pthread_spinlock_t thread_lock;
  int locked;
  fiber_queue(){
    pthread_spin_init(&thread_lock, PTHREAD_PROCESS_PRIVATE);
  }
  void lock(){
    pthread_spin_lock(&thread_lock);
    assert(!locked);
    locked = 1;
  }
  void unlock(){
    assert(locked);
    locked = 0;
    pthread_spin_unlock(&thread_lock);
  }
  bool empty(){
    assert(locked);
    return q.empty();
  }
  int size(){
    assert(locked);
    return q.size();
  }
  func_t** push(f){
    assert(locked);
    q.push_back(0);
    return &*(q.end()-1)
  }
  void push(func_t* f){
    assert(locked);
    q.push(f);
  }
  func_t* pop(){
    assert(!q.empty());
    assert(locked);
    func_t* f = q[0];
    q.pop_front();
    return f;
  }
};









struct resume_context {
  func_t* function;
  fiber_queue* queue_to_join;
};

// A paused fiber, not waiting on any arguments
typedef SWAPSTACK resume_context (*paused_fiber)(fiber_queue*);




// queue_to_join must be locked
void switch_to(paused_fiber fiber, fiber_queue* queue_to_join){
  resume_context ctx = fiber(queue_to_join);
  ctx.queue_to_join->push(ctx.function);
  ctx.queue_to_join->unlock();
}

// both queues must be locked
void switch_to(fiber_queue* queue_to_schedule, fiber_queue* queue_to_join){
  assert(!queue_to_schedule->empty() && "empty runqueue!");
  paused_fiber fib = (paused_fiber)queue_to_schedule->pop();
  queue_to_schedule->unlock();
  switch_to(fib, queue_to_join);
}






struct worker;
pthread_mutex_t global_worker_mutex = PTHREAD_MUTEX_INITIALIZER;
vector<worker*> all_workers;
int nworkers_working = 0;
__thread worker* current_worker;


typedef void (fiber_func)(int);
void fiber_new(fiber_func* func, int arg);


struct worker{
  fiber_queue runnable;
  fiber_queue dead;
  int id;
  void loop(){
    while (1){
      runnable.lock();
      if (runnable.empty()){
        // must unlock self before stealing to prevent
        // deadlock between a den of theives.
        runnable.unlock();
        paused_fiber stolen = steal_work();
        runnable.lock();
        switch_to(stolen, &runnable);
      }else{
        switch_to((paused_fiber)runnable.pop(), &runnable);
      }
    }
  }

  worker(int i) : id(i){
    pthread_mutex_lock(&global_worker_mutex);
    all_workers.push_back(this);
    nworkers_working++;
    pthread_mutex_unlock(&global_worker_mutex);
  }

  worker* find_worker(){
    int nworkers = all_workers.size();
    int start = rand() % nworkers;
    for (int i = (start + 1)%nworkers; i != start; i = (i+1)%nworkers){
      worker* w = all_workers[i];
      if (w == this) continue;
      w->runnable.lock();
      if (!w->runnable.empty()){
        return w;
      }
    }
    if (nworkers_working == 0){
      assert(0);
    }
    return NULL;
  }
  
  paused_fiber steal_work(){
    say("trying to steal\n");
    pthread_mutex_lock(&global_worker_mutex);

    say("%d working\n", nworkers_working);
    nworkers_working--;

    worker* victim = find_worker();
    if (!victim){
      say("no work to steal!\n");
      while (!victim){
        pthread_mutex_unlock(&global_worker_mutex);
        asm volatile("pause\n");
        pthread_mutex_lock(&global_worker_mutex);
        victim = find_worker();
      }
    }
    assert(victim != this);
    paused_fiber stolen = (paused_fiber)victim->runnable.pop();
    victim->runnable.unlock();
    say("stole work\n");

    nworkers_working++;

    pthread_mutex_unlock(&global_worker_mutex);
    return stolen;
  }

  struct worker_init{
    worker* this_;
    fiber_func* mainfiber;
  };
  static void spawn_worker(pthread_t& thread, int id, fiber_func* mainfiber){
    worker_init* wi = new worker_init;
    wi->this_ = new worker(id);
    wi->mainfiber = mainfiber;
    pthread_create(&thread, 0, worker::worker_threadfunc, (void*)wi);
  }
  static void spawn_and_join_workers(int nworkers, fiber_func* mainfiber){
    assert(nworkers > 0);
    pthread_t* threads = new pthread_t[nworkers];
    spawn_worker(threads[0], 0, mainfiber);
    for (int i=1; i<nworkers; i++) spawn_worker(threads[i], i, 0);
    for (int i=0; i<nworkers; i++) pthread_join(threads[i], 0);
    delete[] threads;
  }
  static void* worker_threadfunc(void* worker_init_){
    worker_init* wi = (worker_init*)worker_init_;
    assert(current_worker == 0);
    current_worker = wi->this_;
    if (wi->mainfiber){
      fiber_new(wi->mainfiber, 0);
    }
    wi->this_->loop();
    current_worker = 0;
    delete wi;
    return NULL;
  }
};


// queue_to_join must be locked
void fiber_pause_current(fiber_queue* queue_to_join){
  current_worker->runnable.lock();
  switch_to(&current_worker->runnable, queue_to_join);
}


// current_worker->runnable must be locked
void fiber_add_runnable(paused_fiber fiber){
  current_worker->runnable.push((func_t*)fiber);
}


void fiber_yield(){
  current_worker->runnable.lock();
  if (current_worker->runnable.empty()){
    // continue in this thread if there's nothing to yield to
    return;
  }
  paused_fiber fib = (paused_fiber)current_worker->runnable.pop();
  switch_to(fib, &current_worker->runnable);
}


struct mutex{
  fiber_queue waiting;
  bool locked;
  mutex() : locked(false) {}

  void lock(){
    waiting.lock();
    if (locked){
      fiber_pause_current(&waiting);
    }else{
      locked = true;
      waiting.unlock();
    }
  }

  void unlock(){
    waiting.lock();
    assert(locked);
    if (waiting.empty()){
      locked = false;
      waiting.unlock();
    }else{
      // Two options when release a contended mutex:

      //  1. Pause the current fiber and schedule a waiter
      current_worker->runnable.lock();
      switch_to(&waiting, &current_worker->runnable);

      //  2. Mark a waiter as runnable and continue in this fiber
      // current->runnable.push(m->waiting.pop_front());
    }
  }
};




SWAPSTACK void fiber_init_thunk(func_t fib, fiber_queue* queue, fiber_func* func, int arg){
  say("spawned\n");

  queue->push(fib);
  queue->unlock();
  func(arg);
  current_worker->dead.lock();
  say("dying\n");
  fiber_pause_current(&current_worker->dead);
  say("zombie?\n");
}
#define STACKSIZE 409600
// Creates and switches to a new fiber
// Leaves current fiber on the runqueue
void fiber_new(fiber_func* func, int arg){
  void* stack = malloc(STACKSIZE);
  typedef SWAPSTACK resume_context (*new_fiber_t)(fiber_queue*, fiber_func*, int);
  new_fiber_t fiber = (new_fiber_t)__builtin_newstack(stack, STACKSIZE, (void*)fiber_init_thunk);
  current_worker->runnable.lock();
  say("spawning\n");
  resume_context ctx = fiber(&current_worker->runnable, func, arg);
  ctx.queue_to_join->push(ctx.function);
  ctx.queue_to_join->unlock();
}




template <class T>
struct exchanger{
  struct exchange_data{
    T data1;
    T data2_loc;
  };
  struct exchanger_ret{
    func_t* function;
    exchange_data* dstruct;
  };
  typedef resume_context (exchanger_fn)(T data);
  
  typedef SWAPSTACK exchanger_ret (*exchange_fn_1)(fiber_queue*);

  typedef SWAPSTACK resume_context (*exchange_fn_2)(exchange_data* d);

  fiber_queue waiter;

  T exchange(T value){
    waiter.lock();
    if (!waiter.empty()){
      assert(waiter.size() == 1);
      exchange_fn_2 peer = (exchange_fn_2)waiter.pop();
      waiter.unlock();

      exchange_data dstruct;
      dstruct.data1 = value;
      resume_context ctx = peer(&dstruct);
      ctx.queue_to_join->push(ctx.function);
      ctx.queue_to_join->unlock();
      
      return dstruct.data2_loc;
    }else{
      
      current_worker->runnable.lock();
      assert(!current_worker->runnable.empty()); //FIXME
      exchange_fn_1 newfib = (exchange_fn_1)current_worker->runnable.pop();
      
      current_worker->runnable.unlock();
      exchanger_ret ctx = newfib(&waiter);

      ctx.dstruct->data2_loc = value;
      
      current_worker->runnable.lock();

      current_worker->runnable.push(ctx.function);
      current_worker->runnable.unlock();

      return ctx.dstruct->data1;
    }
  }
};




template <class T>
struct blocking_queue{

  struct writer_sleep_ret{
    func_t* function;
    T* dataptr;
  };
  typedef SWAPSTACK writer_sleep_ret (*writer_sleep)(fiber_queue*);
  typedef SWAPSTACK resume_context (*writer_send)(T);
  struct reader_sleep_ret{
    func_t* function;
    T data;
  };
  typedef SWAPSTACK reader_sleep_ret (*reader_sleep)(fiber_queue*);
  typedef SWAPSTACK resume_context (*reader_receive)(T*);


  enum WAIT_MODE {WAIT_READ, WAIT_WRITE};
  WAIT_MODE wait_mode;
  fiber_queue waiters;
  void write(T data){
    waiters.lock();
    if (wait_mode == WAIT_READ && !waiters.empty()){
      // there is a corresponding reader to wake
      writer_send peer = (writer_send)waiters.pop();
      waiters.unlock();
      assert(peer);
      resume_context ctx = peer(data);
      ctx.queue_to_join->push(ctx.function);
      ctx.queue_to_join->unlock();
    }else{
      // current fiber must wait
      wait_mode = WAIT_WRITE;
      
      current_worker->runnable.lock();
      writer_sleep sleep = (writer_sleep)current_worker->runnable.pop();
      current_worker->runnable.unlock();
      writer_sleep_ret ctx = sleep(&waiters);
      *ctx.dataptr = data;
      current_worker->runnable.lock();
      current_worker->runnable.push(ctx.function);
      current_worker->runnable.unlock();
    }
  }

  T read(){
    waiters.lock();
    if (wait_mode == WAIT_WRITE && !waiters.empty()){
      // there is a corresponding writer to wake
      reader_receive peer = (reader_receive)waiters.pop();
      waiters.unlock();
      T data;
      resume_context ctx = peer(&data);
      ctx.queue_to_join->push(ctx.function);
      ctx.queue_to_join->unlock();
      return data;
    }else{
      // current fiber must wait
      wait_mode = WAIT_READ;
      current_worker->runnable.lock();
      reader_sleep sleep = (reader_sleep)current_worker->runnable.pop();
      current_worker->runnable.unlock();
      reader_sleep_ret ctx = sleep(&waiters);
      current_worker->runnable.lock();
      current_worker->runnable.push(ctx.function);
      current_worker->runnable.unlock();
      return ctx.data;      
    }
  }
};


template <class Req, class Rep>
struct request_reply{
  enum WAIT_MODE {WAIT_REQUEST, WAIT_ACCEPT};
  WAIT_MODE wait_mode;
  fiber_queue waiters;
  // the outstanding_requests queue is a bit of a hack
  queue<Req> outstanding_requests;

  struct request_ret{
    func_t* function;
    Rep rep;
  };

  typedef SWAPSTACK request_ret (*request_send)(Req);
  typedef SWAPSTACK request_ret (*request_sleep)(fiber_queue*);

  struct accept_sleep_ret{
    func_t* function;
    Req req;
  };
  typedef SWAPSTACK accept_sleep_ret (*accept_sleep)(fiber_queue*);

  typedef SWAPSTACK resume_context (*response_send)(Rep);


  typedef func_t* request_cookie;

  Rep request(Req req){
    waiters.lock();
    request_ret ctx;
    if (wait_mode == WAIT_ACCEPT && !waiters.empty()){
      // there is a fiber waiting in accept, wake it
      request_send rqsend = (request_send)waiters.pop();
      waiters.unlock();
      ctx = rqsend(req);
    }else{
      // we must sleep until a responder is available
      wait_mode = WAIT_REQUEST;
      current_worker->runnable.lock();
      request_sleep sleep = (request_sleep)current_worker->runnable.pop();
      current_worker->runnable.unlock();
      outstanding_requests.push(req);
      ctx = sleep(&waiters);
    }
    current_worker->runnable.lock();
    current_worker->runnable.push(ctx.function);
    current_worker->runnable.unlock();
    return ctx.rep;
  }
  
  Req accept(request_cookie& cookie){
    waiters.lock();
    if (wait_mode == WAIT_REQUEST && !waiters.empty()){
      // there is a fiber waiting in request
      cookie = (request_cookie)waiters.pop();
      Req req = outstanding_requests.front();
      outstanding_requests.pop();
      waiters.unlock();
      return req;
    }else{
      // sleep until a request is made
      wait_mode = WAIT_ACCEPT;
      current_worker->runnable.lock();
      accept_sleep sleep = (accept_sleep)current_worker->runnable.pop();
      current_worker->runnable.unlock();
      accept_sleep_ret ctx = sleep(&waiters);
      cookie = (request_cookie)ctx.function;
      return ctx.req;
    }
  }

  void respond(request_cookie cookie, Rep response){
    resume_context ctx = ((response_send)cookie)(response);
    ctx.queue_to_join->push(ctx.function);
    ctx.queue_to_join->unlock();
  }
};



// FIXME untested
struct cyclic_barrier{
  int size;
  cyclic_barrier(int s) : size(s) {}
  fiber_queue waiters;
  void await(){
    waiters.lock();
    if (waiters.size() < size - 1){
      fiber_pause_current(&waiters);
    }else{
      current_worker->runnable.lock();
      while (!waiters.empty()){
        current_worker->runnable.push(waiters.pop());
      }
      current_worker->runnable.unlock();
      waiters.unlock();
    }
  }
};




exchanger<int> ab;
exchanger<int> bc;

void fiber_a(int){
  for (int i=0; i<5; i++){
    int x = i;
    say("[a] sending %d\n", x);
    x = ab.exchange(x);
    say("[a] received %d\n", x);
  }
}

void fiber_b(int){
  int i = -1000;
  while(1){
    say("[b] sending %d to [a]\n", i);
    int j = ab.exchange(i);
    say("[b] got %d from [a], sending to [c]\n", j);
    i = bc.exchange(j);
  }
}

void fiber_c(int){
  int i = 42;
  while (1){
    say("[c] sending %d to [b]\n", i);
    int j = bc.exchange(i);
    say("[c] received %d\n", j);
  }
}



blocking_queue<int> q;
void queue_writer(int){
  for (int i=0; i<20; i++){
    say("[W] writing %d\n", i);
    q.write(i);
  }
}

void queue_reader(int id){
  say("[R%d] starting\n", id);
  while (1){
    int x = q.read();
    say("[R%d] read %d\n", id, x);
  }
}



request_reply<int, int> rq;
void requestor(int id){
  say("{REQ%d} starting\n", id);
  int len[] = {8, 3};
  int start[] = {0, 100};
  for (int i=start[id]; i<start[id]+len[id]; i++){
    say("{REQ%d} requesting %d\n", id, i);
    int rep = rq.request(i);
    say("{REQ%d} got %d from %d\n", id, rep, i);
  }
}
void responder(int){
  request_reply<int,int>::request_cookie cookie;
  while (1){
    int r = rq.accept(cookie);
    say("{REP} responding to %d\n", r);
    rq.respond(cookie, r + 100000);
  }
}
void proxy(int){
  request_reply<int,int>::request_cookie cookie;
  while (1){
    int r = rq.accept(cookie);
    say("{PROXY} proxying %d\n", r);
    r = rq.request(r);
    say("{PROXY} got %d\n", r);
    rq.respond(cookie, -r);
  }
}





mutex mtx;

void yieldmany(int id, int ycount){
  for (int i=0;i<ycount;i++){
    say("<MTX%d> yielding\n", id);
    fiber_yield();
  }
}

void mutexfunc(int id){
  int ycount1[] = {5,3};
  int ycount2[] = {3,10};
  yieldmany(id, ycount1[id]);
  say("<MTX%d> wants lock\n", id);
  mtx.lock();
  say("<MTX%d> has lock\n", id);
  yieldmany(id, ycount2[id]);
  say("<MTX%d> releasing lock\n", id);
  mtx.unlock();
  say("<MTX%d> released lock\n", id);
  yieldmany(id, ycount1[id]);
}


void main_fiber(int){
  fiber_new(mutexfunc,0);
  fiber_new(mutexfunc,1);
  fiber_new(fiber_a,0);
  fiber_new(fiber_b,0);
  fiber_new(fiber_c,0);
  fiber_new(queue_writer,0);
  fiber_new(queue_reader,0);
  fiber_new(queue_reader,1);
  fiber_new(requestor, 0);
  fiber_new(responder,0);
  fiber_new(proxy, 0);
  fiber_new(requestor,1);
}


int main(){
  setvbuf(stdout, 0, _IONBF, 0);
  worker::spawn_and_join_workers(2, main_fiber);
}
