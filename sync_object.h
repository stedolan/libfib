#ifndef SYNC_OBJECT_H
#define SYNC_OBJECT_H

#ifndef FIBER_TYPES_ONLY
#include <cassert>
#include "sched.h"
#endif

#include "llq_ctypes.h"


enum dispatch_method{
  D_SINGLETHREAD,
  D_CONCURRENT
};


struct fiber_sync_object{

  struct llq_queue queue;
  llq_word control;

#ifndef FIBER_TYPES_ONLY

  enum {CONCURRENT = 0x100, UNUSED = 0xffffff, OWNER_START=0x1000000};

  fiber_sync_object(word tag = 0){
    init(tag);
  }
  void init(word tag = 0){
    control = UNUSED;
    //    control = OWNER_START + worker::current().id;
    //llq::spin_init(&control);
    llq::queue_init(&queue, tag);
  }
#define DISPMETH -1
  dispatch_method begin_operation(){

    word OWNED_SELF = OWNER_START + worker::current().id;

#if DISPMETH == -1
    return D_SINGLETHREAD;
#elif DISPMETH == 0
    word ctrl = llq::load_relaxed(&control);

    if (ctrl == OWNER_START + worker::current().id){
      // owned by us
      worker::current().stats.owned++;
    }else if (ctrl == UNUSED && llq::cas(&control, UNUSED, OWNED_SELF)){
      // unowned, we took it
    }else{
      ctrl = llq::load_relaxed(&control);
      assert(ctrl & OWNER_START);
      assert(ctrl != OWNED_SELF);
      // owned by another worker, migrate
      int other = ctrl & ~OWNER_START;
      worker::current().migrate(other);
    }
    return D_SINGLETHREAD;
#elif DISPMETH == 1
    worker::current().stats.conc++; return D_CONCURRENT;
#elif DISPMETH == 2
    word ctrl = llq::load_relaxed(&control);
    if (ctrl == OWNED_SELF){
      // object is owned by current worker
      say("LOCKS[%p] already owned\n", this);
      worker::current().stats.owned++;
      return D_SINGLETHREAD;
    }else if (ctrl == UNUSED && llq::cas(&control, UNUSED, OWNED_SELF)){
      // object is taken by current worker
      say("LOCKS[%p]: taken\n", this);
      return D_SINGLETHREAD;
    }else{
      if (ctrl & OWNER_START){
        // object is owned by some other worker
        int other = ctrl & ~OWNER_START;
        say("LOCKS[%p]: Need, but owned by %d\n", this, other);
        worker::current().migrate(other);
        // control field may have changed while we migrated
        word newctrl = llq::load_relaxed(&control);
        assert(newctrl == ctrl || newctrl == CONCURRENT);
        // in theory, we need only store if it's not already
        // in state CONCURRENT. But it's faster not to branch.
        llq::store_relaxed(&control, CONCURRENT);
        say("LOCKS[%p]: Made concurrent\n", this);
      }else{
        say("LOCKS[%p]: concurrent\n", this);
      }
      worker::current().stats.conc++;
      // object is shared
      return D_CONCURRENT;

    }
#endif    
  }
  void end_operation(){
  }

#endif

};





#ifndef FIBER_TYPES_ONLY









struct mutex{
  fiber_sync_object sync;
  enum { UNLOCKED, LOCKED };
  template <class ops>
  static void lock_impl(llq::queue* q){
    using llq::word;
    while (1){
      word state = ops::get_state(q);
      if (ops::tag(q, state) == LOCKED){
        waiter<void> waiter;
        if (ops::try_enqueue(q, &waiter, state, LOCKED)){
          worker::current().sleep(waiter);
          return;
        }
      }else{
        if (ops::try_transition(q, state, LOCKED)){
          return;
        }
      }
    }
  }
  static void lock(fiber_sync_object& sync){
    dispatch_method meth = sync.begin_operation();
    if (meth == D_SINGLETHREAD) lock_impl<single_thread_ops>(&sync.queue);
    else                        lock_impl<concurrent_ops>(&sync.queue);
    sync.end_operation();
  }
  void lock(){
    lock(sync);
  }

  template <class ops>
  static waiter<void>* unlock_getwaiter_impl(llq::queue* q){
    using llq::word;
    while (1){
      word state = ops::get_state(q);
      assert(ops::tag(q, state) == LOCKED);
      if (ops::isempty(q, state)){
        if (ops::try_transition(q, state, UNLOCKED)){
          // No waiter
          return 0;
        }
      }else{
        llq::node* waiting;
        if (ops::try_dequeue(q, &waiting, state, LOCKED)){
          return static_cast<waiter<void>* >(waiting);
        }
      }
    }
  }

  template <class ops>
  static void unlock_impl(llq::queue* q){
    waiter<void>* w = unlock_getwaiter_impl<ops>(q);
    if (w){
      // FIXME: maybe wake rather than sched?
      worker::current().push_runqueue(w);
    }
  }
  static void unlock(fiber_sync_object& sync){
    dispatch_method meth = sync.begin_operation();
    if (meth == D_SINGLETHREAD) unlock_impl<single_thread_ops>(&sync.queue);
    else                        unlock_impl<concurrent_ops>(&sync.queue);
    sync.end_operation();
  }
  void unlock(){
    unlock(sync);
  }
  static waiter<void>* unlock_getwaiter(fiber_sync_object& sync){
    dispatch_method meth = sync.begin_operation();
    if (meth == D_SINGLETHREAD) return unlock_getwaiter_impl<single_thread_ops>(&sync.queue);
    else                        return unlock_getwaiter_impl<concurrent_ops>(&sync.queue);
    sync.end_operation();
  }
  waiter<void>* unlock_getwaiter(){
    return unlock_getwaiter(sync);
  }
};


struct condition_var{
  fiber_sync_object sync;
  template <class ops>
  static void wait_impl(llq::queue* q, fiber_sync_object& mtx){
    using llq::word;
    waiter<void> w;
    while (1){
      word state = ops::get_state(q);
      if (ops::try_enqueue(q, &w, state, 0)){
        break;
      }
    }
    waiter<void>* lockwaiter = mutex::unlock_getwaiter(mtx);
    if (lockwaiter){
      w.invoke(lockwaiter);
    }else{
      worker::current().sleep(w);
    }
    mutex::lock(mtx);
  }
  static void wait(fiber_sync_object& sync, fiber_sync_object& mtx){
    dispatch_method meth = sync.begin_operation();
    if (meth == D_SINGLETHREAD) wait_impl<single_thread_ops>(&sync.queue, mtx);
    else                        wait_impl<concurrent_ops>(&sync.queue, mtx);
    sync.end_operation();
  }
  void wait(mutex* mtx){
    wait(sync, mtx->sync);
  }
  
  template <class ops>
  static void signal_impl(llq::queue* q, bool wake_all){
    using llq::word;
    while (1){
      word state = ops::get_state(q);
      llq::node* waiting;
      if (ops::isempty(q, state)){
        // Nobody listening, signal went nowhere
        break;
      }else if (ops::try_dequeue(q, &waiting, state, 0)){
        // FIXME: wake vs. sched
        // FIXME: this could be faster: push to mutexq instead of runq
        worker::current().push_runqueue(static_cast<waiter<void>* >(waiting));
        if (!wake_all) break;
      }
    }
  }
  static void signal(fiber_sync_object& sync){
    dispatch_method meth = sync.begin_operation();
    if (meth == D_SINGLETHREAD) signal_impl<single_thread_ops>(&sync.queue, false);
    else                        signal_impl<single_thread_ops>(&sync.queue, false);
    sync.end_operation();
  }
  void signal(){
    signal(sync);
  }

  static void broadcast(fiber_sync_object& sync){
    dispatch_method meth = sync.begin_operation();
    if (meth == D_SINGLETHREAD) signal_impl<single_thread_ops>(&sync.queue, true);
    else                        signal_impl<single_thread_ops>(&sync.queue, true);
    sync.end_operation();
  }
  void broadcast(){
    broadcast(sync);
  }

};




template <class T>
struct blocking_channel{
  fiber_sync_object sync;

  struct writer_wait : public waiter<void>{
    T data;
  };

  enum { WAIT_WRITE, WAIT_READ };

  template <class ops>
  static void write_impl2(llq::queue* q, T data) NOINLINE{
    printf("IN2\n");
    using llq::word;
    while (1){
      word state = ops::get_state(q);
      if (ops::isempty(q, state) || ops::tag(q, state) == WAIT_WRITE){
        writer_wait w;
        w.data = data;
        if (ops::try_enqueue(q, &w, state, WAIT_WRITE)){
          worker::current().sleep(w);
          return;
        }
      }else{
        node* peer_;
        if (ops::try_dequeue(q, &peer_, state, 0)){
          waiter<void> self;
          // FIXME read_func should happen earlier, maybe?
          worker::current().push_runqueue(&self);
          self.invoke(static_cast<waiter<T>* >(peer_), data);
          return;
        }
      }
    }
  }

  template <class ops>
  static void write_impl(llq::queue* q, T data){
    using llq::word;
    /*while (1)*/{
      word state = ops::get_state(q);
      if (ops::isempty(q, state) || ops::tag(q, state) == WAIT_WRITE){
        writer_wait w;
        w.data = data;
        if (ops::try_enqueue(q, &w, state, WAIT_WRITE)){
          worker::current().sleep(w);
          return;
        }
      }else{
        node* peer_;
        if (ops::try_dequeue(q, &peer_, state, 0)){
          waiter<void> self;
          // FIXME read_func should happen earlier, maybe?
          worker::current().push_runqueue(&self);
          self.invoke(static_cast<waiter<T>* >(peer_), data);
          return;
        }
      }
    }
    write_impl2<ops>(q, data);
  }

  template <class ops>
  static T read_impl(llq::queue* q){
    using llq::word;
    while (1){
      word state = ops::get_state(q);
      if (ops::isempty(q, state) || ops::tag(q, state) == WAIT_READ){
        waiter<T> self;
        if (ops::try_enqueue(q, &self, state, WAIT_READ)){
          waiter<void>* waiting = worker::current().pop_runqueue();
          return self.invoke(waiting);
        }
      }else{
        llq::node* peer_;
        if (ops::try_dequeue(q, &peer_, state, 0)){
          // We must read the data before we add the writer to the runqueue
          // in case the writer gets work-stolen and the data deallocated
          writer_wait* peer = static_cast<writer_wait*>(peer_);
          T data = peer->data;
          // FIXME barriers? #LoadStore ?
          // FORKPOINT
          worker::current().push_runqueue(peer);
          return data;
        }
      }
    }
  }


  void write(T data){
    dispatch_method meth = sync.begin_operation();
    if (meth == D_SINGLETHREAD) write_impl<single_thread_ops>(&sync.queue, data);
    else                        write_impl<concurrent_ops>(&sync.queue, data);
    sync.end_operation();
  }
  T read(){
    dispatch_method meth = sync.begin_operation();
    T result;
    if (meth == D_SINGLETHREAD) result = read_impl<single_thread_ops>(&sync.queue);
    else                        result = read_impl<concurrent_ops>(&sync.queue);
    sync.end_operation();
    return result;
  }
};


template <class T>
struct blocker{
  fiber_sync_object sync;
  struct blocker_wait : public waiter<void>{
     T data;
  };

  enum { WAIT_BLOCK, WAIT_ACCEPT };

  template <class ops>
  static void block_impl(llq::queue* q, T data){
    using llq::word;
    blocker_wait w;
    w.data = data;
    while (1){
      word state = ops::get_state(q);
      if (ops::isempty(q, state)){
        if (ops::try_enqueue(q, &w, state, WAIT_BLOCK)){
          worker::current().sleep(w);
          return;
        }
      }else{
        assert(ops::tag(q, state) == WAIT_ACCEPT);
        node* peer_;
        if (ops::try_dequeue(q, &peer_, state, 0)){
          waiter<blocker_wait*>* peer = static_cast<waiter<blocker_wait*>* >(peer_);
          w.invoke(peer, &w);
          return;
        }
      }
    }
  }

  template <class ops>
  static blocker_wait* accept_impl(llq::queue* q){
    using llq::word;
    while (1){
      word state = ops::get_state(q);
      if (ops::isempty(q, state)){
        waiter<blocker_wait*> self;
        say("Accept is waiting: %p\n", &self);
        if (ops::try_enqueue(q, &self, state, WAIT_ACCEPT)){
          return worker::current().sleep(self);
        }
      }else{
        assert(ops::tag(q, state) == WAIT_BLOCK);
        llq::node* peer_;
        if (ops::try_dequeue(q, &peer_, state, 0)){
          return static_cast<blocker_wait*>(peer_);
        }
      }
    }
  }


  void block(T data){
    dispatch_method meth = sync.begin_operation();
    if (meth == D_SINGLETHREAD) block_impl<single_thread_ops>(&sync.queue, data);
    else                        block_impl<concurrent_ops>(&sync.queue, data);
    sync.end_operation();
  }
  blocker_wait* accept(){
    dispatch_method meth = sync.begin_operation();
    blocker_wait* result;
    if (meth == D_SINGLETHREAD) result = accept_impl<single_thread_ops>(&sync.queue);
    else                        result = accept_impl<concurrent_ops>(&sync.queue);
    sync.end_operation();
    return result;
  }
};






#if 0
template <class T>
struct actor{
  fiber_sync_object sync;
  void send(T data){
    dispatch_method meth = sync.begin_operation();
    if (meth == D_SINGLETHREAD) send_impl<single_thread_ops>(data);
    else                        send_impl<concurrent_ops>(data);
    sync.end_operation();
  }
  T receive(){
    dispatch_method meth = sync.begin_operation();
    T result;
    if (meth == D_SINGLETHREAD) result = receive_impl<single_thread_ops>();
    else                        result = receive_impl<concurrent_ops>();
    sync.end_operation();
    return result;
  }

  enum { ACTOR_RECEIVE, ACTOR_RUN };

  
  struct wdata{
    func_t** loc;
    T data;
  };
  typedef SWAPSTACK resume_context (*actor_direct_send)(wdata*);

  template <class ops>
  static void send_impl(llq::queue* q, T data){
    using llq::word;
    while (1){
      word state = ops::get_state(q);
      if (ops::tag(q, state) == ACTOR_RECEIVE){
        waiter* actor;
        waiter selfsleep;
        if (ops::try_dequeue(q, &actor, state, ACTOR_RUN)){
          worker::current.push_runqueue(&self);
          wdata msg = {&self.func, data};
          actor_direct_send actorsend = (actor_direct_send)actor->read_func();
          post_context_switch(actorsend(&msg));
        }
      }else{
        if (ops::try_enqueue(q, &w, 
      }
    }
  }
};

#endif






#endif // FIBER_TYPES_ONLY

#endif // header guard
