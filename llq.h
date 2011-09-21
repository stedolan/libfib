#ifndef LLQ_H
#define LLQ_H
#include <assert.h>
#include "llq_ctypes.h"



#define BEGIN_NS namespace llq{
#define END_NS }
BEGIN_NS

// Import llq_ctypes into the llq:: namespace
typedef llq_word word;
typedef llq_ctptr ctptr;
typedef llq_node node;
typedef llq_queue queue;


#define COMPILER_BARRIER asm volatile("" ::: "memory")

#ifdef LLQ_X86_64
#define CPU_RELAX asm volatile ("\tpause\n" ::: "memory")
// X86 has an implicit #LoadStore | #StoreStore | #LoadLoad
// before and after every instruction
// Only #StoreLoad requires an MFENCE instruction

// #LoadStore | #StoreStore
#define STORE_RELEASE_BARRIER 

// #LoadLoad | #LoadStore
#define LOAD_ACQUIRE_BARRIER

#define FULL_BARRIER asm volatile \
  ( "mfence" ::: "memory" )
#endif
#ifdef LLQ_PPC64
#define CPU_RELAX asm volatile (           \
  "\tor 1,1,1\n" /* low SMT priority */    \
  "\tor 2,2,2\n" /* medium SMT priority */ \
  ::: "memory")

// FIXME barriers could be weaker?
#define STORE_RELEASE_BARRIER asm volatile \
  ( "isync" ::: "memory" )
#define LOAD_ACQUIRE_BARRIER asm volatile \
  ( "isync" ::: "memory" )
#define FULL_BARRIER asm volatile \
  ( "sync" ::: "memory" )
#endif


#ifdef LLQ_X86_64
static word atomic_xchg(volatile word* ptr, word oldval){
  __asm__ __volatile__ 
    (
     "\txchg %0, %1\n"
     : "+r"(oldval), "+m"(*ptr)
     :
     : "memory"
    );
  return oldval;
}
#endif

static void store_release(volatile word* ptr, word val){
  STORE_RELEASE_BARRIER;
  *ptr = val;
}

static word load_acquire(volatile word* ptr){
  word val = *ptr;
  LOAD_ACQUIRE_BARRIER;
  return val;
}

static word load_relaxed(volatile word* ptr){
  return *ptr;
}

static void store_relaxed(volatile word* ptr, word val){
  *ptr = val;
}


//FIXME
static word read_and_catch_fault(word){return 0;}

static word read_hazard(volatile word* ptr){
  return (word)read_and_catch_fault((word)ptr);
}


// Full memory barrier
static bool cas(volatile word* ptr, word oldval, word newval){
  return __sync_bool_compare_and_swap(ptr, oldval, newval);
}

#ifdef LLQ_X86_64
// Full memory barrier
static bool dcas(volatile word* ptr, word old0, word old1, word new0, word new1){
  bool result;
  // The "memory" element in the clobber-list is not necessary for cmpxchg16b
  // but it causes the instruction to become a compiler barrier as well.
  __asm__ __volatile__
    (
     // On X86-64, no explicit memory barriers are needed around locked instructions
     "\tlock cmpxchg16b %1\n"
     "\tsetz %0\n"
     : "=q" (result), "+m" (*ptr), "+a" (old0), "+d" (old1)
     : "b" (new0), "c" (new1)
     : "cc", "memory"
    );
  return result;
}
#endif

#ifdef LLQ_PPC64

static word load_linked(volatile word* ptr){
  word val;
  __asm__ __volatile__
    (
     "\tldarx %0,0,%1\n"
     : "=r"(val)
     : "r"(ptr)
     : "memory"
    );
  return val;
}
static bool store_conditional(volatile word* ptr, word value){
  word success;
  __asm__ __volatile__
    (
     "\tstdcx. %1,0,%2\n"
     "\tli %0,0\n"
     "\tbne- 1f\n"
     "\tli %0,1\n"
     "1:\n"
     : "=r" (success)
     : "r" (value), "r" (ptr)
     : "cr0", "memory"
    );
  return (bool)success;
}
#endif

// Full memory barrier
static word fetch_and_inc(volatile word* ptr){
  return __sync_fetch_and_add(ptr, 1);
}
// Full memory barrier
static word fetch_and_dec(volatile word* ptr){
  return __sync_fetch_and_sub(ptr, 1);
}

// Full memory barrier
static word fetch_and_inc2(volatile word* ptr){
  return __sync_fetch_and_add(ptr, 100);
}
// Full memory barrier
static word fetch_and_dec2(volatile word* ptr){
  return __sync_fetch_and_sub(ptr, 100);
}


#ifdef LLQ_HAS_DCAS
#define LLQ_HAS_UPDATE_CTPTR 1
static bool update_ctptr(volatile ctptr* ptr, ctptr oldval, word newval){
  word
    old0 = oldval.ptr, old1 = oldval.serial,
    new0 = newval,     new1 = oldval.serial + 1;
  return dcas((volatile word*)ptr, old0, old1, new0, new1);
}
#endif

#if 0
static bool try_acquire_lock(volatile word* lock){
#if defined(LLQ_HAS_ATOMIC_XCHG)
  word old = atomic_xchg(lock, 1);
  if (old == 0) return true;
  else return false;
#elif defined(LLQ_PPC64)
  word tmp, token;
  __asm__ __volatile__(
        // FIXME use hint in ldarx
"1:	ldarx           %0,0,%2\n\
	cmpwi		0,%0,0\n\
	bne-		2f\n\
	stwcx.		%1,0,%2\n\
	bne-		1b\n\
        isync           \n"
	PPC_ACQUIRE_BARRIER
"2:"
	: "=&r" (tmp)
	: "r" (token), "r" (&lock->slock)
	: "cr0", "memory");
#endif
}


static void spin_lock(volatile word* lock){
  bool success = try_acquire_lock(lock);
  while (!success){
    do {
      CPU_RELAX;
    } while (*lock);
    success = try_acquire_lock(lock);
  }
}

static bool spin_is_locked(volatile word* lock){
  return *lock == 1;
}

static void spin_unlock(volatile word* lock){
  assert(spin_is_locked(lock));
  store_release(lock, 0);
}

static void spin_init(word* lock){
  *lock = 0;
}
#endif


// Creates a new object, giving it whole cachelines
#define LINESIZE 64
template <class T>
static inline T* cachealign_new(){
  size_t rounded = ((sizeof(T) + (LINESIZE-1)) / LINESIZE) * LINESIZE;
  assert(rounded % LINESIZE == 0);
  assert(rounded >= sizeof(T));
  void* mem = malloc(rounded + LINESIZE);
  word w = (word)mem;
  w += LINESIZE - (w % LINESIZE);
  assert(w % LINESIZE == 0);
  T* x = new ((void*)w) T;
  return x;
}

static void declare_initialisable(word* ptr){
  store_relaxed(ptr, 0);
  LOAD_ACQUIRE_BARRIER;//FIXME
}

static void signal_initialisation(word* ptr, word val){
  store_release(ptr, val);
}

static word await_initialisation(word* ptr){
  word val = load_acquire(ptr);
#ifdef FIBER_SINGLETHREADED
  assert(val);
  return val;
#else
  if (val) return val;
#ifndef NDEBUG
  fprintf(stderr, "Slow value %p\n", ptr);
#endif
  while (!val){
    CPU_RELAX;
    val = load_acquire(ptr);
  } while(!val);
  return val;
#endif
}

static node* node_next(node* n){
  return (node*)await_initialisation((word*)&n->next);
}


#define PTR_TAG_MASK 0x3 // Low 2 bits of pointer are tag bits

static void assert_valid_tag(word tag){
  assert((tag & ~PTR_TAG_MASK) == 0);
}

template <class T>
static T* unwrap_tagptr(word tagged){
  return (T*)(tagged & ~PTR_TAG_MASK);
}

static word tagptr_tag(word tagged){
  return tagged & PTR_TAG_MASK;
}

template <class T>
static word wrap_tagptr(T* ptr, word tag){
  assert_valid_tag(tag);
  return (word)ptr | tag;
}

static node* queue_head_ptr(queue* q){
  // struct 3ctptr is layout-compatible with struct node
  return &q->head;
}


static word q_get_state(queue* q){
  return q->tail.ptr;
}
static word state_isempty(queue* q, word p){
  return unwrap_tagptr<node>(p) == &q->head;
}

static void queue_init(queue* q, word tag){
  q->head.next = 0;
#ifdef LLQ_HAS_DCAS
  q->tail.serial = 0;
#endif
  q->tail.ptr = wrap_tagptr(&q->head, tag);
}

static void ST_enqueue(queue* q, node* newnode, word oldtail, word newtag){
  assert_valid_tag(newtag);
  
  node* tailptr = unwrap_tagptr<node>(oldtail);
  newnode->next = 0;
  tailptr->next = newnode;
  q->tail.ptr = wrap_tagptr(newnode, newtag);
}

static void ST_dequeue(queue* q, node** nodeptr, word oldtail, word newtag_ifempty){
  assert_valid_tag(newtag_ifempty);
  node* head = queue_head_ptr(q);
  assert(unwrap_tagptr<node>(oldtail) != head);
  node* first = head->next;
  q->head.next = first->next;
  if (unwrap_tagptr<node>(oldtail) == first){
    // dequeued last element, must move tail
    q->tail.ptr = wrap_tagptr(queue_head_ptr(q), newtag_ifempty);
  }
  *nodeptr = first;
}

static void ST_transition(queue* q, word oldtail, word newtag){
  assert(q->tail.ptr == oldtail);
  q->tail.ptr = wrap_tagptr(unwrap_tagptr<node>(oldtail), newtag);
}

#if LLQ_HAS_UPDATE_CTPTR
static bool MT_try_enqueue(queue* q, node* newnode, word oldtail, word newtag){
  // FIXME: race? barrier? wtf?
  newnode->next = 0;
  // Load the other half of the tail pointer
  ctptr oldtailc = {oldtail, q->tail.serial};
  // Swing the tail pointer to the new node
  bool success = update_ctptr(&q->tail, oldtailc, wrap_tagptr(newnode, newtag));
  if (!success) return false;

  // If a producer blocks at this point until the queue empties, 
  // a consumer will spin waiting for the store on the next line.

  // Update the old tail's "next" pointer
  assert(unwrap_tagptr<node>(oldtail)->next == 0);
  unwrap_tagptr<node>(oldtail)->next = newnode;
  return true;
}
#elif LLQ_HAS_LL_SC
static bool MT_try_enqueue(queue* q, node* newnode, word oldtail, word newtag){
  assert(q->tail.ptr);
  // barrierr????
  declare_initialisable((word*)&newnode->next);
  FULL_BARRIER;

  word tail = load_linked(&q->tail.ptr);
  FULL_BARRIER;
  if (tail != oldtail) return false; // FIXME weaken
    
  if (store_conditional(&q->tail.ptr, wrap_tagptr(newnode, newtag))){
    FULL_BARRIER;
    assert(unwrap_tagptr<node>(oldtail)->next == 0);
    FULL_BARRIER;
    signal_initialisation((word*)(&unwrap_tagptr<node>(oldtail)->next), (word)newnode);
    FULL_BARRIER;
    return true;
  }
  return false;
}
#endif

#if LLQ_HAS_UPDATE_CTPTR
// FIXME: possible race condition with the last element
// (queue has 1 element, enqueue and dequeue happen simultaneously)
static bool MT_SP_try_enqueue(queue* q, node* newnode, word oldtail, word newtag){
  // FIXME memory ordering
  store_release((word*)&newnode->next, 0);
  node* oldtailptr = unwrap_tagptr<node>(oldtail);
  store_release((word*)&oldtailptr->next, (word)newnode);
  store_release((word*)&q->tail, wrap_tagptr(newnode, newtag));
  return true;
}
#endif


#if LLQ_HAS_UPDATE_CTPTR
static bool MT_try_dequeue(queue* q, node** nodeptr, word oldtail, word newtag_ifempty){
  node* head = queue_head_ptr(q);
  if (unwrap_tagptr<node>(oldtail) == head){
    // Queue is empty
    return false;
  }

  // We try to set the head pointer from non-zero to zero.
  // This excludes other consumers (like a spin-lock), and also waits for the
  // producer to have stored a pointer into the head.
  node* first = (node*)atomic_xchg((word*)head, 0);
  
  while (first == 0){
    // spin until we acquire the "consumer lock"
    do{
      CPU_RELAX;
      // queue may have become empty, reload tail
      // FIXME BUG we should fail if oldtail changes a lot
      oldtail = q->tail.ptr; // FIXME barriers?
      if (unwrap_tagptr<node>(oldtail) == head){
        return false;
      }
    }while (!head->next); // FIXME barriers?
    first = (node*)atomic_xchg((word*)head, 0);
  }

  // Here we know that: we're the only consumer running, and the
  // producer has already filled in head->next (so the queue is non-empty).
  
  assert(unwrap_tagptr<node>(q->tail.ptr) != head);
  
  // If the queue is one element long, we must leave the head pointer as 0
  // and set the tail pointer = &head.
  // If the queue is longer, we must set the head pointer to head->next.
  // The queue may change from one-element to longer at any moment, but cannot
  // change back (as we're the only consumer currently running).
  ctptr oldtailc = q->tail;
  if (unwrap_tagptr<node>(oldtailc.ptr) == first
      &&
      update_ctptr(&q->tail, oldtailc, 
                   wrap_tagptr(queue_head_ptr(q), newtag_ifempty))){
    // The queue had only one element and we successfully removed it
    
    // we need not set head to 0 as it is already 0
    *nodeptr = first;
    return true;
  }else{
    // The queue has more than one element
    node* next = (node*)await_initialisation((word*)(&first->next));
    store_release((word*)head, (word)next);
    *nodeptr = first;
    return true;
  }
}
#elif LLQ_HAS_LL_SC
static bool MT_try_dequeue(queue* q, node** nodeptr, word oldtail, word newtag_ifempty){
  assert(q->tail.ptr);
  node* head = queue_head_ptr(q);
  // FIXME is this necessary? change to assert?
  if (unwrap_tagptr<node>(oldtail) == head){
    // Queue is empty
    return false;
  }

  node* first = (node*)load_linked((word*)head);
  while (first == 0){
    do {
      CPU_RELAX;
      oldtail = q->tail.ptr;
      assert(oldtail);
      if (unwrap_tagptr<node>(oldtail) == head){
        return false;
      }
    }while (!head->next);
    first = (node*)load_linked((word*)head);
  }
  
  if (store_conditional((word*)head, 0)){
    assert(unwrap_tagptr<node>(q->tail.ptr) != head);
    // we have the consumer lock
    word currtail = load_relaxed(&q->tail.ptr);
    if (currtail != oldtail){
      store_release((word*)head, (word)first);
      return false;
    }
    if (unwrap_tagptr<node>(currtail) == first){
      word currtail2 = load_linked(&q->tail.ptr);
      if (currtail2 == currtail &&
          store_conditional(&q->tail.ptr, 
                            wrap_tagptr(head, newtag_ifempty))){
        *nodeptr = first;
        return true;
      }else{
        // failed, unlock head
        store_release((word*)head, (word)first);
        return false;
      }
    }
    
    node* next = node_next(first);
    store_release((word*)head, (word)next);
    *nodeptr = first;
    return true;
  }
  return false;
}

#endif

#ifdef LLQ_HAS_UPDATE_CTPTR
// FIXME: tag seems racy
static bool MT_try_flush(queue* q, node** firstptr, node** lastptr, word oldtail, word newtag_ifempty){
  node* head = queue_head_ptr(q);
  if (unwrap_tagptr<node>(oldtail) == head){
    // Queue is empty
    return false;
  }

  // We try to set the head pointer from non-zero to zero.
  // This excludes other consumers (like a spin-lock), and also waits for the
  // producer to have stored a pointer into the head.
  node* first = (node*)atomic_xchg((word*)head, 0);
  
  while (first == 0){
    // spin until we acquire the "consumer lock"
    do{
      CPU_RELAX;
      // queue may have become empty, reload tail
      oldtail = q->tail.ptr; // FIXME barriers?
      if (unwrap_tagptr<node>(oldtail) == head){
        return false;
      }
    }while (!head->next); // FIXME barriers?
    first = (node*)atomic_xchg((word*)head, 0);
  }

  // Here we know that: we're the only consumer running, and the
  // producer has already filled in head->next (so the queue is non-empty).
  
  assert(unwrap_tagptr<node>(q->tail.ptr) != head);
  
  ctptr oldtailc = q->tail;
  node* last = unwrap_tagptr<node>(oldtailc.ptr);
  if (update_ctptr(&q->tail, oldtailc, 
                   wrap_tagptr(queue_head_ptr(q), newtag_ifempty))){
    // We got the whole queue
    
    // we need not set head to 0 as it is already 0
    *firstptr = first;
    *lastptr = last;
    return true;
  }else{
    // The queue has been extended
    node* next = (node*)await_initialisation((word*)(&last->next));
    store_release((word*)head, (word)next);
    *firstptr = first;
    *lastptr = last;
    return true;
  } 
}
#endif

#ifdef LLQ_HAS_UPDATE_CTPTR
static bool MT_try_transition(queue* q, word oldtail, word newtag){
  ctptr oldtailc = {oldtail, q->tail.serial};
  if (update_ctptr(&q->tail, oldtailc,
                   wrap_tagptr(unwrap_tagptr<node>(oldtail), newtag))){
    return true;
  }else{
    return false;
  }
}
#elif LLQ_HAS_LL_SC
static bool MT_try_transition(queue* q, word oldtail, word newtag){
  word newt = wrap_tagptr(unwrap_tagptr<node>(oldtail), newtag);
  
  word t = load_linked(&q->tail.ptr);
  if (t != oldtail) return false;
  return store_conditional(&q->tail.ptr, newt);
}
#endif

struct single_thread_ops{
  static void begin(queue* q){
  }
  static void end(queue* q){
  }

  // Convenience functions when single-threaded and not using tags
  static bool enqueue(queue* q, node* nodeptr){
    return try_enqueue(q, nodeptr, get_state(q), 0);
  }
  static node* head(queue* q){
    assert(!isempty(q, get_state(q)));
    return queue_head_ptr(q)->next;
  }
  static node* tail(queue* q){
    assert(!isempty(q, get_state(q)));
    return unwrap_tagptr<node>(q->tail.ptr);
  }

  static bool try_enqueue(queue* q, node* nodeptr, word oldstate, word newtag){
    ST_enqueue(q, nodeptr, oldstate, newtag);
    return true;
  }
  static bool try_dequeue(queue* q, node** nodeptr, word oldstate, word newtag_ifempty){
    ST_dequeue(q, nodeptr, oldstate, newtag_ifempty);
    return true;
  }
  static bool try_transition(queue* q, word oldstate, word newtag){
    ST_transition(q, oldstate, newtag);
    return true;
  }
  static word get_state(queue* q){
    return q_get_state(q);
  }
  static bool isempty(queue* q, word state){
    return state_isempty(q, state);
  }
  static bool tag(queue* q, word state){
    return tagptr_tag(state);
  }
};
#if defined(FIBER_SINGLETHREADED)

typedef single_thread_ops concurrent_ops;
#elif LLQ_HAS_UPDATE_CTPTR || LLQ_HAS_LL_SC
// DCAS-based concurrent queue
struct concurrent_ops{
  static void begin(queue* q){
  }
  static void end(queue* q){
  }
  static bool try_enqueue(queue* q, node* nodeptr, word oldstate, word newtag){
    return MT_try_enqueue(q, nodeptr, oldstate, newtag);
  }
  static bool try_dequeue(queue* q, node** nodeptr, word oldstate, word newtag_ifempty){
    return MT_try_dequeue(q, nodeptr, oldstate, newtag_ifempty);
  }
  static bool try_transition(queue* q, word oldstate, word newtag){
    return MT_try_transition(q, oldstate, newtag);
  }
  static word get_state(queue* q){
    return q_get_state(q);
  }
  static bool isempty(queue* q, word state){
    return state_isempty(q, state);
  }
  static bool tag(queue* q, word state){
    return tagptr_tag(state);
  }
};
#else
// spinlock-based queue
struct concurrent_ops{
  static void begin(queue* q){
    //spin_lock(&q->tail.serial);
  }
  static void end(queue* q){
    //spin_unlock(&q->tail.serial);
  }
  static void assert_locked(queue* q){
    //assert(spin_is_locked(&q->tail.serial));
  }
  static bool try_enqueue(queue* q, node* nodeptr, word oldstate, word newtag){
    assert_locked(q);
    return single_thread_ops::try_enqueue(q,nodeptr,oldstate,newtag);
  }
  static bool try_dequeue(queue* q, node** nodeptr, word oldstate, word newtag_ifempty){
    assert_locked(q);
    return single_thread_ops::try_dequeue(q, nodeptr, oldstate, newtag_ifempty);
  }
  static bool try_transition(queue* q, word oldstate, word newtag){
    assert_locked(q);
    return single_thread_ops::try_transition(q, oldstate, newtag);
  }
  static word get_state(queue* q){
    assert_locked(q);
    return single_thread_ops::get_state(q);
  }
  static bool isempty(queue* q, word state){
    assert_locked(q);
    return single_thread_ops::isempty(q,state);
  }
  static bool tag(queue* q, word state){
    assert_locked(q);
    return single_thread_ops::tag(q,state);
  }
};
#endif


END_NS
#endif
