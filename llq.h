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
#define CPU_RELAX asm volatile ("\tpause\n" ::: "memory")

// X86 has an implicit #LoadStore | #StoreStore | #LoadLoad
// before and after every instruction
// Only #StoreLoad requires an MFENCE instruction

// #LoadStore | #StoreStore
#define STORE_RELEASE_BARRIER 

// #LoadLoad | #LoadStore
#define LOAD_ACQUIRE_BARRIER

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



static bool update_ctptr(volatile ctptr* ptr, ctptr oldval, word newval){
  word
    old0 = oldval.ptr, old1 = oldval.serial,
    new0 = newval,     new1 = oldval.serial + 1;
  return dcas((volatile word*)ptr, old0, old1, new0, new1);
}

static void spin_lock(volatile word* lock){
  word old = atomic_xchg(lock, 1);
  while (old != 0){
    do {
      CPU_RELAX;
    } while (*lock);
    old = atomic_xchg(lock, 1);
  }
}

static void spin_unlock(volatile word* lock){
  assert(*lock == 1);
  store_release(lock, 0);
}

static void spin_init(word* lock){
  *lock = 0;
}


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


static word await_initialisation(word* ptr){
  word val = load_relaxed(ptr);
  if (1||val) return val;
#ifndef NDEBUG
  printf("Slow value %p\n", ptr);
#endif
  while (!val){
    CPU_RELAX;
    val = load_relaxed(ptr);
  } while(!val);
  return val;
}

static node* node_next(node* n){
  return (node*)await_initialisation((word*)&n->next);
}


#define PTR_TAG_MASK 0x3 // Low 2 bits of pointer are tag bits

static void assert_valid_tag(word tag){
  assert((tag & ~PTR_TAG_MASK) == 0);
}

static node* unwrap_tagptr(word tagged){
  return (node*)(tagged & ~PTR_TAG_MASK);
}
static word tagptr_tag(word tagged){
  return tagged & PTR_TAG_MASK;
}
static word wrap_tagptr(node* ptr, word tag){
  assert_valid_tag(tag);
  return (word)ptr | tag;
}

static node* queue_head_ptr(queue* q){
  // struct ctptr is layout-compatible with struct node
  return &q->head;
}


static word q_get_state(queue* q){
  return q->tail.ptr;
}
static word state_isempty(queue* q, word p){
  return unwrap_tagptr(p) == &q->head;
}

static void queue_init(queue* q, word tag){
  q->head.next = 0;
  q->tail.serial = 0;
  q->tail.ptr = wrap_tagptr(&q->head, tag);
}

static void ST_enqueue(queue* q, node* newnode, word oldtail, word newtag){
  assert_valid_tag(newtag);
  
  node* tailptr = unwrap_tagptr(oldtail);
  newnode->next = 0;
  tailptr->next = newnode;
  q->tail.ptr = wrap_tagptr(newnode, newtag);
}

static void ST_dequeue(queue* q, node** nodeptr, word oldtail, word newtag_ifempty){
  assert_valid_tag(newtag_ifempty);
  node* head = queue_head_ptr(q);
  assert(unwrap_tagptr(oldtail) != head);
  node* first = head->next;
  q->head.next = first->next;
  if (unwrap_tagptr(oldtail) == first){
    // dequeued last element, must move tail
    q->tail.ptr = wrap_tagptr(queue_head_ptr(q), newtag_ifempty);
  }
  *nodeptr = first;
}

static void ST_transition(queue* q, word oldtail, word newtag){
  q->tail.ptr = wrap_tagptr(unwrap_tagptr(oldtail), newtag);
}

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
  assert(unwrap_tagptr(oldtail)->next == 0);
  unwrap_tagptr(oldtail)->next = newnode;
  return true;
}

// FIXME: possible race condition with the last element
// (queue has 1 element, enqueue and dequeue happen simultaneously)
static bool MT_SP_try_enqueue(queue* q, node* newnode, word oldtail, word newtag){
  // FIXME memory ordering
  store_release((word*)&newnode->next, 0);
  node* oldtailptr = unwrap_tagptr(oldtail);
  store_release((word*)&oldtailptr->next, (word)newnode);
  store_release((word*)&q->tail, wrap_tagptr(newnode, newtag));
  return true;
}


static bool MT_try_dequeue(queue* q, node** nodeptr, word oldtail, word newtag_ifempty){
  node* head = queue_head_ptr(q);
  if (unwrap_tagptr(oldtail) == head){
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
      if (unwrap_tagptr(oldtail) == head){
        return false;
      }
    }while (!head->next); // FIXME barriers?
    first = (node*)atomic_xchg((word*)head, 0);
  }

  // Here we know that: we're the only consumer running, and the
  // producer has already filled in head->next (so the queue is non-empty).
  
  assert(unwrap_tagptr(q->tail.ptr) != head);
  
  // If the queue is one element long, we must leave the head pointer as 0
  // and set the tail pointer = &head.
  // If the queue is longer, we must set the head pointer to head->next.
  // The queue may change from one-element to longer at any moment, but cannot
  // change back (as we're the only consumer currently running).
  ctptr oldtailc = q->tail;
  if (unwrap_tagptr(oldtailc.ptr) == first
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

// FIXME: tag seems racy
static bool MT_try_flush(queue* q, node** firstptr, node** lastptr, word oldtail, word newtag_ifempty){
  node* head = queue_head_ptr(q);
  if (unwrap_tagptr(oldtail) == head){
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
      if (unwrap_tagptr(oldtail) == head){
        return false;
      }
    }while (!head->next); // FIXME barriers?
    first = (node*)atomic_xchg((word*)head, 0);
  }

  // Here we know that: we're the only consumer running, and the
  // producer has already filled in head->next (so the queue is non-empty).
  
  assert(unwrap_tagptr(q->tail.ptr) != head);
  
  ctptr oldtailc = q->tail;
  node* last = unwrap_tagptr(oldtailc.ptr);
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

static bool MT_try_transition(queue* q, word oldtail, word newtag){
  ctptr oldtailc = {oldtail, q->tail.serial};
  if (update_ctptr(&q->tail, oldtailc,
                   wrap_tagptr(unwrap_tagptr(oldtail), newtag))){
    return true;
  }else{
    return false;
  }
}


END_NS
#endif
