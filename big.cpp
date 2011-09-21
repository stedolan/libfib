#include <vector>
#include <functional>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <utility>
#include <queue>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <poll.h>
#include "llq.h"
#include "sched.h"
#include "io.h"
using std::vector;
using std::pair;
using std::make_pair;
using std::priority_queue;


namespace timestamp{
  timestamp now(){
    struct timespec tv;
    clock_gettime(CLOCK_MONOTONIC, &tv);
    return ((timestamp)tv.tv_nsec) + ((timestamp)tv.tv_sec) * T_SECONDS;
  }
}



static void mark_nonblocking(int fd){
  int flags = fcntl(fd, F_GETFL);
  if (!(flags & O_NONBLOCK)){
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

struct io_manager;



struct fd_handler{
  int fd;
  enum { LAST_READ_BLOCKED  = 1,
         LAST_WRITE_BLOCKED = 2 };
  uint32_t selector_state;
  llq::queue waiters;

  fd_handler(int fd_) : fd(fd_), selector_state(0) {
    llq::queue_init(&waiters, 0);
  }
  
  bool check_retry(io_manager* mng, opcode op){
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS){
      waiter<void> w;
      typedef llq::single_thread_ops ops;
      while (!ops::try_enqueue(&waiters, &w,
                               ops::get_state(&waiters), 0));
      mng->add_waiter(this, op);
      worker::current().sleep(w);
      return true;
    }else{
      return false;
    }
  }

  
  int connect(io_manager* mng, const struct sockaddr* addr, socklen_t addrlen){
    int r;
    do{
      r = ::connect(fd, addr, addrlen);
    } while (r < 0 && errno == EINTR);

    // wait until either success or an error other than EINPROGRESS
    while (check_retry(mng, OP_CONNECT)){
      int sockerr;
      socklen_t sz = sizeof(sockerr);
      ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &sz);
      errno = sockerr;
      r = sockerr ? -1 : 0;
    }

    return r < 0 ? -errno : r;
  }

  int accept(io_manager* mng, struct sockaddr* addr, socklen_t* addrlen){
    // FIXME: this could use accept4 on Linux and save a syscall
    while (1){
      int r;
      do{
        r = ::accept(fd, addr, addrlen);
      } while (r < 0 && errno == EINTR);
      if (r < 0){
        if (!check_retry(mng, OP_ACCEPT))
          return -errno;
        else
          continue;
      }else{
        mng->setup_fd(r);
        return r;
      }
    }
  }
  ssize_t read(io_manager* mng, void* buf, size_t count){
    return perform_rdwr(mng, buf, count, OP_READ);
  }
  ssize_t write(io_manager* mng, const void* buf, size_t count){
    return perform_rdwr(mng, (void*)buf, count, OP_WRITE);
  }
  
  ssize_t perform_rdwr(io_manager* mng, void* buf, size_t count, opcode op){
    while (1){
      int r;
      do{
        switch (op){
        case OP_READ:  r = ::read(fd, buf, count); break;
        case OP_WRITE: r = ::write(fd, buf, count); break;
        default: assert(0);
        }
      } while (r < 0 && errno == EINTR);
      
      if (r < 0){
        if (!check_retry(mng, op))
          return -errno;
        else
          continue;
      }else{
        return r;
      }
    }
  }

  void wake(llq::queue* wokenq, opcode event){
    // wake all
    // FIXME only wake some?
    typedef llq::single_thread_ops ops;
    while (1){
      llq::word tag = ops::get_state(&waiters);
      if (ops::isempty(&waiters, tag)) break;
      llq::node* w;
      if (ops::try_dequeue(&waiters, &w, tag, 0)){
        ops::enqueue(wokenq, w);
      }
    }
  }
};


struct poll_selector{
  enum { TIMER_RESOLUTION = timestamp::T_MILLISECONDS };
  enum { TIMEOUT_POLL = 0, TIMEOUT_WAIT = -1 };
  vector<struct pollfd> pollfds;
  io_manager* mng;
  poll_selector(io_manager* m) : mng(m){}
  void add_ops(struct pollfd& pfd, opcode ops){
    if (ops & OP_READ)    pfd.events |= POLLIN;
    if (ops & OP_WRITE)   pfd.events |= POLLOUT;
    if (ops & OP_ACCEPT)  pfd.events |= POLLIN;
    if (ops & OP_CONNECT) pfd.events |= POLLIN | POLLOUT;
  }
  void add_fd(fd_handler* h, opcode ops){
    if (h->selector_state == 0){
      // fd not previously seen, append
      h->selector_state = (pollfds.size() << 2) | ops;
      struct pollfd pfd;
      pfd.fd = h->fd;
      pfd.events = 0;
      pfd.revents = 0;
      add_ops(pfd, ops);
      pollfds.push_back(pfd);
    }else if ((h->selector_state & ops) != ops){
      // fd seen with a subset of ops, update
      struct pollfd& pfd = pollfds[h->selector_state >> 2];
      add_ops(pfd, ops);
    }
  }
  int select(llq::queue* wokenq, timestamp::timediff timeout){
    assert(timeout > 0 || timeout == TIMEOUT_WAIT || timeout == TIMEOUT_POLL);
    if (timeout == TIMEOUT_WAIT && pollfds.empty()){
      // Nothing will ever happen no matter how long we wait
      return 0;
    }
    int millis = timeout != TIMEOUT_WAIT ? timeout / timestamp::T_MILLISECONDS : -1;
    int res;
    do {
      res = poll(&pollfds[0], pollfds.size(), millis);
    } while(res < 0 && errno == EINTR);
    if (res < 0) return -errno;
    int woken = 0;
    int nout = 0;
    for (int i=0; i<pollfds.size(); i++){
      opcode ev = (opcode)
        (((pollfds[i].revents & POLLIN ) ? OP_READ  : 0) | 
         ((pollfds[i].revents & POLLOUT) ? OP_WRITE : 0));
      if (ev){
        fd_handler* h = mng->get_handler(pollfds[i].fd);
        h->selector_state = 0;
        h->wake(wokenq, ev);
        woken++;
      }else{
        // keep pollfd for next time
        pollfds[nout++] = pollfds[i];
      }
    }
    assert(nout + woken == pollfds.size());
    pollfds.resize(nout);
    if (woken == 0) assert(res == 0);
    return woken;
  }
  bool has_outstanding_io(){
    return !pollfds.empty();
  }
};

struct selector{
  typedef poll_selector curr_selector;
  curr_selector s;
  selector(io_manager* m) : s(m) {}
};

io_manager::io_manager(worker* w) 
  : sel(new selector(this)), current_worker(w) {}
io_manager::~io_manager() { delete sel; }

fd_handler* io_manager::get_handler(int fd){

  if (handlers.size() < fd + 1){
    handlers.resize(fd + 1, 0);
  }
  if (!handlers[fd])
    handlers[fd] = new fd_handler(fd);

  return handlers[fd];
}

void io_manager::setup_fd(int fd){
  mark_nonblocking(fd);
  if (handlers.size() < fd + 1){
    handlers.resize(fd + 1, 0);
  }
  handlers[fd] = new fd_handler(fd);
}
  
void io_manager::sleep_for(timestamp::timediff duration){
  printf("sleeping for %lld\n", (long long int)duration);
  sleep_until(timestamp::now() + duration);
}
void io_manager::sleep_until(timestamp::timestamp when){
  waiter<void> w;
  timers.push(make_pair(when, &w));
  worker::current().sleep(w);
}

int io_manager::wake_all_until(llq::queue* woken, timestamp::timestamp when){
  int nwoken = 0;
  while (!timers.empty() && timers.top().first <= when){
    printf("Waking!!! %.2f secs out\n", ((double)timers.top().first - when) / timestamp::T_SECONDS);
    llq::single_thread_ops::enqueue(woken, timers.top().second);
    timers.pop();
    nwoken++;
  }
  return nwoken;
}
int io_manager::perform_io(llq::queue* wokenq){
  assert(current_worker == &worker::current());
  timestamp::timestamp now = timestamp::now();
  wake_all_until(wokenq, now);
  int woken;
  if (!timers.empty()){
    timestamp::timestamp firsttimer = timers.top().first;
    assert(firsttimer > now);
    printf("First timer is %.2f secs away\n", ((double)firsttimer - now) / timestamp::T_SECONDS);
    woken = sel->s.select(wokenq, firsttimer - now);
    if (woken == 0)
      woken = wake_all_until(wokenq, firsttimer + 
                             selector::curr_selector::TIMER_RESOLUTION/2);
  }else{
    woken = sel->s.select(wokenq, selector::curr_selector::TIMEOUT_WAIT);
    if (woken == 0) assert(!sel->s.has_outstanding_io());
  }
  return woken;
}
bool io_manager::has_outstanding_io(){
  return !timers.empty() || !sel->s.has_outstanding_io();
}

int io_manager::connect(int fd, const struct sockaddr* addr, socklen_t addrlen){
  return get_handler(fd)->connect(this, addr, addrlen);
}
int io_manager::accept(int fd, struct sockaddr* addr, socklen_t* addrlen){
  return get_handler(fd)->accept(this, addr, addrlen);
}
ssize_t io_manager::read(int fd, void* buf, size_t count){
  return get_handler(fd)->read(this, buf, count);
}
ssize_t io_manager::write(int fd, const void* buf, size_t count){
  return get_handler(fd)->write(this, buf, count);
}

int io_manager::close(int fd){
  get_handler(fd);
  handlers[fd] = 0;
  int r;
  do{
    r = close(fd);
  } while (r < 0 && errno == EINTR);
  return r;
}

void io_manager::add_waiter(fd_handler* h, opcode op){
  sel->s.add_fd(h, op);
}

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
  assert(worker::current_() == 0);
  worker::current_() = wi->this_;
  say("  running\n");
  if (wi->mainfiber){
    waiter<void> w;
    w.next = &worker::sentinel;
    worker::current().local_reserved = &w;
    worker::current().new_fiber(wi->mainfiber, 0, &w.func);
  }
  worker::current().scheduler_loop();
  say("  worker terminating\n");
  assert(worker::current().thisthread == pthread_self());
  worker::current_() = 0;
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
  assert(worker::current_() == thisworker);
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
  say("  main-thread worker terminating\n");
  worker::current_() = 0;
  
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
  worker::current_() = all_workers[0];
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
  assert(!worker::current_());
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
llq::node worker::sentinel = { 0 };
#ifndef LLQ_PPC64
__thread worker* current_worker_;
#endif
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <utility>
using namespace std;
#include "actor.h"
#include "spawn.h"

#define COUNT 5000000



class Printer : public Actor<Printer>{
public:
  enum MessageType { MSG };
  typedef Message<Printer, MSG, int> Msg;
  void run(){
    //for (int i=1; i<=COUNT; i++){
      Msg* m = (Msg*)receive();
      printf("Got %d\n", m->payload);
      delete m;
      //}
  }
};


/*
class Incrementor : public Actor<Incrementor>{
public:
  enum MessageType {MSG};
  typedef Message<Incrementor, MSG, pair<int, int> > Msg;
  void run(){
    while (1){
      Msg* msg = (Msg*)receive();
      pair<int, int> p = msg->payload;
      p.first += 2;
      p.second--;
      delete msg;
      if (p.second == 0){
        final->send(new Printer::Msg(p.first));
      }else{
        next->send(new Incrementor::Msg(p));
      }
    }
  }
  Actor<Incrementor>* next;
  Actor<Printer>* final;
};*/


Printer::Msg* finalmsg = new Printer::Msg;

class Incrementor : public Actor<Incrementor>{
public:
  enum MessageType {MSG};
  typedef Message<Incrementor, MSG, pair<int, int> > Msg;
  void run(){
    while (1){
      Msg* msg = (Msg*)receive();
      pair<int, int> p = msg->payload;
      p.first += 2;
      p.second--;
      msg->payload = p;
      if (p.second == 0){
        finalmsg->payload = p.first;
        final->send(finalmsg);
      }else{
        next->send(msg);
      }
    }
  }
  Actor<Incrementor>* next;
  Actor<Printer>* final;
};


class Doubler : public Actor<Doubler>{
public:
  enum MessageType {MSG};
  typedef RequestMessage<Doubler, MSG, int, int> Msg;
  void run(){
    while (1){
      printf("D: waiting\n");
      Msg& msg = receive()->as<Msg>();
      printf("D: Doubling %d\n", msg.payload*2);
      msg.reply(msg.payload * 2);
    }
  }
};

class Requestor : public Actor<Requestor>{
public:
  Doubler* dbl;
  int id;
  void run(){
    for (int i=0; i<100; i++){
      printf("R%d: req %d\n", id, i);
      Doubler::Msg msg(i);
      int reply = dbl->request(&msg);
      printf("R%d: rep %d\n", id, reply);
    }
  }
};




template <class ActorT>
int actor_thunk(ActorT* act){
  act->run();
  return 0;
}

template <class ActorT>
void actor_thunk_v(ActorT* act){
  act->run();
}



void spawn_incrementors(int n, int msg){
  Incrementor* incs = new Incrementor[n];
  Printer* p = new Printer;
  fiber_handle<int> pt = spawn_fiber_fixed<int, Printer*, actor_thunk<Printer> >(p, 102400);
#define SS 256
  char* stacks = (char*)new void*[n * (SS / sizeof(void*))];
  for (int i=0; i<n; i++){
    incs[i].next = i == n-1 ? &incs[0] : &incs[i+1];
    incs[i].final = p;
    spawn_fiber_fixed_detached<Incrementor*, actor_thunk_v<Incrementor> >(&incs[i], SS, (void*)&stacks[i*SS]);
  }
  incs[0].send(new Incrementor::Msg(make_pair(0, msg)));
  pt.join();
}




int main(int argc, char** argv){
  worker::spawn_workers(1);
  spawn_incrementors(atoi(argv[1]), atoi(argv[2]));
  //  spawn_incrementors(503, 500);
  
  //Doubler* dbl = new Doubler;
  //dbl->start();
  //spawn_fiber_fixed<int, Doubler*, actor_thunk<Doubler> >(dbl, 102400);
  /*
  for (int i=0; i<2; i++){
    Requestor* r = new Requestor;
    r->id = i;
    r->dbl = dbl;
    r->start();
    }*/
  /*
  FloatPrinter* fp = new FloatPrinter;

  int (*func)(FloatPrinter*) = actor_thunk<FloatPrinter>;

  FloatSender* fs = new FloatSender(fp);
  fiber_handle<int> fpt = spawn_fiber_fixed<int, FloatPrinter*, actor_thunk<FloatPrinter> >(fp, 1024*3);
  fiber_handle<int> fst = spawn_fiber_fixed<int, FloatSender*, actor_thunk<FloatSender> >(fs, 10543);


  fpt.join();
  fst.join();
  */
  worker::await_completion();
}
