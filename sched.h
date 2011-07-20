#ifndef SCHED_H
#define SCHED_H
#include "llq.h"
#include "switch.h"

using namespace llq;


#define NOINLINE __attribute__((noinline))

struct single_thread_ops{
  static bool try_enqueue(queue* q, node* nodeptr, word oldstate, word newtag){
    llq::ST_enqueue(q, nodeptr, oldstate, newtag);
    return true;
  }
  static bool try_dequeue(queue* q, node** nodeptr, word oldstate, word newtag_ifempty){
    llq::ST_dequeue(q, nodeptr, oldstate, newtag_ifempty);
    return true;
  }
  static bool try_transition(queue* q, word oldstate, word newtag){
    llq::ST_transition(q, oldstate, newtag);
    return true;
  }
  static word get_state(queue* q){
    return llq::q_get_state(q);
  }
  static bool isempty(queue* q, word state){
    return llq::state_isempty(q, state);
  }
  static bool tag(queue* q, word state){
    return llq::tagptr_tag(state);
  }
};
struct concurrent_ops{
  static bool try_enqueue(queue* q, node* nodeptr, word oldstate, word newtag){
    return llq::MT_try_enqueue(q, nodeptr, oldstate, newtag);
  }
  static bool try_dequeue(queue* q, node** nodeptr, word oldstate, word newtag_ifempty){
    return llq::MT_try_dequeue(q, nodeptr, oldstate, newtag_ifempty);
  }
  static bool try_transition(queue* q, word oldstate, word newtag){
    return llq::MT_try_transition(q, oldstate, newtag);
  }
  static word get_state(queue* q){
    return llq::q_get_state(q);
  }
  static bool isempty(queue* q, word state){
    return llq::state_isempty(q, state);
  }
  static bool tag(queue* q, word state){
    return llq::tagptr_tag(state);
  }
};


#define SWAPSTACK __attribute__((swapstack))
typedef void (func_t)(void);

#ifdef QUIET
#define say(...)
#else
#define say(fmt, ...) fprintf(stdout, "WRK%*d%*s: " fmt, \
         1+(int)worker::current().id,                    \
         (int)worker::current().id,                      \
         worker::nworkers - (int)worker::current().id -1,\
         "",                                             \
         ##__VA_ARGS__); //fflush(stdout);
#endif
//#define say(fmt, ...) 



static __attribute__((noinline)) void optbarrier(){COMPILER_BARRIER;}

struct resume_context {
  func_t* function;
  func_t** store_loc;
};

// A paused fiber, not waiting on any arguments
typedef SWAPSTACK resume_context (*paused_fiber)(func_t**);
static void post_context_switch(resume_context ctx){
  *ctx.store_loc = ctx.function;
}


typedef void (fiber_func)(int);



struct worker;
extern __thread worker* current_worker_;


#define STACKSIZE 40960
static SWAPSTACK void fiber_init_thunk(func_t fib, func_t** loc, fiber_func* func, int arg);
struct worker{
  static int nworkers;
  static worker** all_workers;



  static void spawn_and_join_workers(int nworkers, fiber_func* mainfiber);
  static void spawn_workers(int nworkers);
  static void await_completion();



  // Sadly, this is stupid
  static llq::word active_workers;
  void set_active(){
    llq::fetch_and_inc(&worker::active_workers);
  }
  void set_inactive(){
    llq::fetch_and_dec(&worker::active_workers);
  }
  bool is_terminated(){
    return llq::load_relaxed(&worker::active_workers) == 0;
  }
  

  static llq::node sentinel;




  pthread_t thisthread;
  int id;
  int randseed;
  llq::node* local_reserved;
  int lifo_push_count;

  char pad[64];

  llq::queue local;
  llq::queue remote;


  struct worker_stats{
    static void print_header(){
      const char* msgs[] = {"RQ del", "RQ add", "Sched", "Steal", "Scarce", "Sync Owned", "Sync Shared", "Migrations In", "Migrations Out"};
      fprintf(stderr, "    ");
      for (int i=0; i<sizeof(msgs)/sizeof(msgs[0]); i++){
        fprintf(stderr, "%16s", msgs[i]);
      }
      fprintf(stderr, "\n");
    }
    void print_stats(int id){
      int nums[] = {qpops, qpushes, nsched, steals, scarce, owned, conc, migratein, migrateout};
      fprintf(stderr, "WRK%d", id);
      for (int i=0; i<sizeof(nums)/sizeof(nums[0]); i++){
        int parts[] = {nums[i]/1000000000, nums[i]/1000000, nums[i]/1000, nums[i]};
        bool hit = false;
        for (int j=0;j<4;j++){
          if (parts[j] && hit){
            fprintf(stderr, " %03d", parts[j]%1000);
            hit = true;
          }else if (parts[j]){
            fprintf(stderr, " %3d", parts[j]%1000);
            hit = true;
          }else if (j == 3){
            fprintf(stderr, "   3");
          }else{
            fprintf(stderr, "    ");
          }
        }
      }
      fprintf(stderr, "\n");
    }
    int qpops, qpushes, nsched, localres, steals, scarce, owned, conc, migrateout, migratein;
    worker_stats(){ reset();}
    void reset(){ qpops=qpushes=nsched=localres=steals=scarce=owned=conc=migrateout=migratein=0; }
  };
  
  worker_stats stats;

  worker(){
    local_reserved = &sentinel;
    llq::queue_init(&local, 0);
    llq::queue_init(&remote, 0);
    randseed = 35442352 + id * 23243;
  }

  static inline worker& current(){
    return *current_worker_;
  }


  waiter<void>* pop_runqueue(){
    assert(local_reserved);
    assert(local_reserved != &sentinel);
    llq::node* first = local_reserved;
    local_reserved = first->next; //llq::node_next(first);
    stats.qpops++;
    stats.localres++;
    return static_cast<waiter<void>* >(first);
  }

  void push_runqueue_fifo(waiter<void>* w){
    // FIXME: SP enqueue
    while (!llq::MT_try_enqueue(&local, static_cast<node*>(w), 
                                llq::q_get_state(&local), 0));
  }
  void push_runqueue_lifo(waiter<void>* w){
    w->next = local_reserved;
    local_reserved = w;
  }
  void push_runqueue(waiter<void>* w){
    stats.qpushes++;
    if (lifo_push_count > 0){
      lifo_push_count--;
      push_runqueue_lifo(w);
    }else{
      push_runqueue_fifo(w);
    }
  }


  template<class T>
  T sleep(waiter<T>& self){
    waiter<void>* waiting = pop_runqueue();
    return self.invoke(waiting);
  }

  void ctx_switch(func_t** loc, waiter<void>* waiting){
    assert(&waiting->func != loc);
    paused_fiber fiber = (paused_fiber)waiting->read_func();
    resume_context ctx = fiber(loc);
    post_context_switch(ctx);
  }

  void migrate(int targetidx){
    waiter<void> w;
    worker* target = all_workers[targetidx];
    llq::fetch_and_inc2(&worker::active_workers);
    stats.migrateout++;
    waiter<void>* waiting = pop_runqueue();
    while (1){
      llq::word tag = llq::q_get_state(&target->remote);
      if (llq::MT_try_enqueue(&target->remote, &w, tag, 0)){
        w.invoke(waiting);
        break;
      }
    }
  }

  int rand(){
    return randseed++;
  }

  bool try_steal_work(llq::node** firstptr, llq::node** lastptr, bool mark_activity){
    //    say("Stealing work - %d\n", worker::active_workers);
    int start = rand() % nworkers;
    for (int i=((start+1)%nworkers); i!=start; i = (i+1) % nworkers){
      worker* w = all_workers[i];
      if (w == this) continue;
      llq::word tag = llq::q_get_state(&w->local);
      llq::node* nodeptr;
      if (!llq::state_isempty(&w->local, tag)){
        if (mark_activity) set_active();
        say("  going to steal from %d...\n", i);
        if (llq::MT_try_dequeue(&w->local, &nodeptr, tag, 0)){
        //        if (llq::MT_try_flush(&w->local, firstptr, lastptr, tag, 0)){
          say("  ...stolen\n");
          *firstptr = *lastptr = nodeptr;
          return true;
        }else{
          say("  ...failed\n");
        }
        if (mark_activity){
          set_inactive();
          if (is_terminated()) return false;
        }
      }
    }
    return false;
  }
  
  bool try_get_scarce_work(llq::node** newblock, llq::node** newblock_last){
    // Work-stealing failed and we found no work in local or remote queue
    // It may be that there is no work, and we should exit, or it may be
    // a false alarm.
    say("Not much to steal...\n");
    set_inactive();
    stats.scarce++;
    while (1){
      //      stats.scarce++;

      // FIXME dup
      llq::word rtag = llq::q_get_state(&remote);
      if (!llq::state_isempty(&remote, rtag)){
        // FIXME: only dequeuing a single node from 
        // remotequeue penalises migration
        if (llq::MT_try_dequeue(&remote, newblock, rtag, 0)){
          stats.migratein++;
          set_active();
          llq::fetch_and_dec2(&worker::active_workers);
          *newblock_last = *newblock;
          break;
        }
      }else{
        if (try_steal_work(newblock, newblock_last, true)){
          break;
        }
        if (is_terminated()){
          return false;
        }
      }
    }
    return true;
  }

  void scheduler_loop(){
    while (1){
      // This runs whenever the local runqueue empties and we must grab more
      assert(local_reserved == &sentinel);
      stats.nsched++;
      say("localsz %d\n", stats.localres);
      stats.localres = 0;
      say("\n");
      llq::node* newblock;
      llq::node* newblock_last;
      bool stolen = false;

      llq::word rtag = llq::q_get_state(&remote);
      llq::word ltag = llq::q_get_state(&local);
      // Get a new batch of work, from somewhere
      if (!llq::state_isempty(&remote, rtag)
          && 
          llq::MT_try_dequeue(&remote, &newblock, rtag, 0)){
        // Got work from remote queue
        // FIXME: only dequeuing a single node from 
        // remotequeue penalises migration
        stats.migratein++;

        llq::fetch_and_dec2(&worker::active_workers);
        newblock_last = newblock;
      }else if (!llq::state_isempty(&local, ltag)
                &&
                /*llq::MT_try_flush(&local, &newblock, &newblock_last, ltag, 0))*/
                llq::MT_try_dequeue(&local, &newblock, ltag, 0))
        {
          newblock_last = newblock;
        // got some more work from local queue
      }else if (try_steal_work(&newblock, &newblock_last, false)){
        // stole some more work
        stats.steals++;
        stolen = true;
      }else if (try_get_scarce_work(&newblock, &newblock_last)){
        // nearly ran out of work!
        say("Got scarce work\n");
        stolen = true;
      }else{
        // there is no work at all
        return;
      }
    

      // We add the current fiber (the scheduler) to the end of the new
      // local reserved queue so that we run again when the queue is
      // exhausted
      waiter<void> w;
      say("sched %p\n", &w.func);

      w.next = &sentinel;
#ifndef NDEBUG
      for (llq::node* n = newblock; n != newblock_last; n = llq::node_next(n)){
        assert(n);
      }
#endif
      newblock_last->next = &w;
      // essentially this:
      //local_reserved = newblock;
      //sleep(&w.func);
      lifo_push_count = 10;
      local_reserved = llq::node_next(newblock);
      if (stolen) say("  executing new block\n");
      w.invoke(static_cast<waiter<void>* >(newblock));
    }
  }

  void yield(){
    waiter<void> w;
    push_runqueue_fifo(&w);
    waiter<void>* other = pop_runqueue();
    assert(other != &w);
    w.invoke(other);
  }

  // Creates and switches to a new fiber
  // Leaves current fiber on the runqueue
  void new_fiber(fiber_func* func, int arg, func_t** loc, size_t stacksize = STACKSIZE){
    void* stack = malloc(stacksize);
    typedef SWAPSTACK resume_context (*new_fiber_t)(func_t**, fiber_func*, int);
    new_fiber_t fiber = (new_fiber_t)__builtin_newstack(stack, stacksize, (void*)fiber_init_thunk);
    say("spawning\n");
    // FORKPOINT
    resume_context ctx = fiber(loc, func, arg);
    post_context_switch(ctx);
  }

  void new_fiber(fiber_func* func, int arg){
    waiter<void> w;
    worker::current().push_runqueue(&w);
    new_fiber(func, arg, &w.func);
  }


};


static SWAPSTACK void fiber_init_thunk(func_t fib, func_t** loc, fiber_func* func, int arg){
  resume_context ctx = {fib, loc};
  post_context_switch(ctx);

  func(arg);

  waiter<void> deadself;
  say("fiber dying\n");
  worker::current().sleep(deadself); // FIXME proper cleanup
  say("zombie?\n");
  assert(0);
}


#endif
