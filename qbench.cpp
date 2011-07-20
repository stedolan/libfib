#include <pthread.h>
#include <assert.h>
#include <algorithm>
#include <queue>
#include <stdio.h>
#include <unistd.h>
using namespace std;

struct qnode{
  qnode* next;
  int value;
  int id;
};


#define COMPILER_BARRIER asm volatile("" ::: "memory")

#ifdef PQ
#define PERQ __thread
#else
#define PERQ
#endif

void spin_lock(volatile int* l){
  int old = __sync_lock_test_and_set(l, 1);
  //  if (old) spins++;
  //  locks++;
  while (old != 0){
    do {asm volatile ("\tpause\n");} while (*l);
    old = __sync_lock_test_and_set(l, 1);
  }
}
void spin_unlock(volatile int* l){
  
  COMPILER_BARRIER;
  *l = 0;
  COMPILER_BARRIER;
  
  //  __sync_lock_release(&lock, 0);
}

#if defined(ALG0)
void q_setup(){ }
__thread qnode head;
__thread qnode* tail = 0;
void q_enqueue(qnode* node){
  if (!tail) tail = &head;
  node->next = 0;
  tail->next = node;
  tail = node;
}
qnode* q_dequeue(){
  qnode* qn;
  if (head.next == 0){
    qn = 0;
  }else{
    qn = head.next;
    head.next = qn->next;
    if (tail == qn) tail = &head;
  }
  return qn;
}
void q_end(){}
#elif defined(ALG1)
PERQ pthread_mutex_t qmutex;
PERQ queue<qnode*>* q;
void q_setup(){
  pthread_mutex_init(&qmutex, 0);
  q = new queue<qnode*>;
}
void q_end(){}
void q_enqueue(qnode* node){
  pthread_mutex_lock(&qmutex);
  //  printf("ENQ %d %d\n", node->id, node->value);
  q->push(node);
  pthread_mutex_unlock(&qmutex);
}

qnode* q_dequeue(){
  pthread_mutex_lock(&qmutex);
  qnode* qn;
  assert(!q->empty());
  qn = q->front();
  q->pop();
  pthread_mutex_unlock(&qmutex);
  return qn;
}
#elif defined(ALG2)
PERQ pthread_spinlock_t qlock;
PERQ qnode head;
PERQ qnode* tail;
void q_setup(){ tail = &head; pthread_spin_init(&qlock, PTHREAD_PROCESS_PRIVATE); }

void q_enqueue(qnode* node){
  pthread_spin_lock(&qlock);
  node->next = 0;
  tail->next = node;
  tail = node;
  pthread_spin_unlock(&qlock);
}
qnode* q_dequeue(){
  pthread_spin_lock(&qlock);
  qnode* qn;
  if (head.next == 0){
    qn = 0;
  }else{
    qn = head.next;
    head.next = qn->next;
    if (tail == qn) tail = &head;
  }
  pthread_spin_unlock(&qlock);
  return qn;
}
void q_end(){}
#elif defined(ALG3)
PERQ int lock = 0;
PERQ qnode head;
PERQ qnode* tail;
#ifdef DBG
__thread int spins;
__thread int locks;
#endif
void q_setup(){tail = &head;}

void q_enqueue(qnode* node){
  spin_lock(&lock);
  node->next = 0;
  tail->next = node;
  tail = node;
  spin_unlock(&lock);
}
qnode* q_dequeue(){
  spin_lock(&lock);
  qnode* qn;
  if (head.next == 0){
    qn = 0;
  }else{
    qn = head.next;
    head.next = qn->next;
    if (tail == qn) tail = &head;
  }
  spin_unlock(&lock);
  return qn;
}

void q_end(){
  //printf("%.1f%% spins\n", 100.0f * spins / locks);
}
#elif defined(ALG4)
PERQ qnode head_;
#ifdef DBG
__thread int spins;
__thread int locks;
#endif
PERQ volatile qnode* tail_;
void q_setup(){tail_ = &head_;}
void q_enqueue(volatile qnode* node){
  volatile qnode* volatile* tail = &tail_;
  node->next = 0;
  volatile qnode* prev = __sync_lock_test_and_set(tail, node);
  prev->next = (qnode*)node;
}
qnode* q_dequeue(){
  volatile qnode* volatile* tail = &tail_;
  volatile qnode* volatile* head = (volatile qnode* volatile*)&head_.next;

  volatile qnode* t = *tail;
  if (t == &head_) return 0;
  
  volatile qnode* first = __sync_lock_test_and_set(head, (qnode*)0);
  
#ifdef DBG
  locks++;
  if (first == 0) spins++;
#endif
  while (first == 0){
    do {
      asm volatile ("\tpause\n");
      if (*tail == &head_) return 0;
    } while (!*head);
    first = __sync_lock_test_and_set(head, 0);
  }
  assert(*tail != &head_);
  volatile qnode* next;
  if (*tail == first && __sync_bool_compare_and_swap(tail, first, &head_)){
    return (qnode*)first;
  }else{
    next = first->next;
    while (!next){
      next = first->next;
    }
    *head = next;
    return (qnode*)first;
  }
}
void q_end(){
  //printf("%.1f%% spins\n", 100.0f * spins / locks);
}

#elif defined(ALG5)
PERQ qnode head_;
#ifdef DBG
__thread int spins;
__thread int locks;
#endif
PERQ volatile qnode* tail_;
void q_setup(){
  tail_ = &head_;
}

void q_enqueue(volatile qnode* node){
  volatile qnode* volatile* tail = &tail_;
  node->next = 0;
  volatile qnode* prev = __sync_lock_test_and_set(tail, node);
  //  sleep(rand()%5);
  prev->next = (qnode*)node;
}
//extern "C" void* read_and_catch_fault(volatile void*);
static void* read_and_catch_fault(volatile void* p){
  return (void*)*(volatile void*volatile*)p;
}
qnode* q_dequeue(){
  volatile qnode* volatile* tail = &tail_;
  volatile qnode* volatile* head = (volatile qnode* volatile*)&head_.next;

 RETRY:
  if (*tail == &head_) return 0;

#ifdef DBG  
  locks++;
#endif
  volatile qnode* first = *head;
  #ifdef DBG
  if (first == 0) spins++;
  #endif
  while (first == 0){
    asm volatile ("\tpause\n");
    if (*tail == &head_) return 0;
    first = *head;
  }


  if (*tail == first){
    if (__sync_bool_compare_and_swap(head, first, 0)){
      if (__sync_bool_compare_and_swap(tail, first, &head_)){
        // popped from single-element queue
        return (qnode*)first;
      }else{
        // tail moved on
        volatile qnode* next = first->next;
        while (!next) next = first->next;
        *head = next;
        return (qnode*)first;
      }
    }else{
      // some other consumer got the head first
      // or the head hasn't arrived yet (blocking on producer)
      //      spins++;
      goto RETRY;
    }
  }else{
    first = *head;
    if (!first) goto RETRY;
    volatile qnode* next = (volatile qnode*)read_and_catch_fault(&first->next);
    while (!next){
      next = (volatile qnode*)read_and_catch_fault(&first->next);
      if (*tail == first){
        //          spins++;
        goto RETRY;
      }
    }
    if (*head != first){
      goto RETRY;
    }
    if (__sync_bool_compare_and_swap(head, first, next)){
      return (qnode*)first;
    }else{
#ifdef DBG
            spins++;
#endif
      goto RETRY;
    }
  }
  assert(0);
}
void q_end(){
  //  printf("%.1f%% spins\n", 100.0f * spins / locks);
}

#elif defined(ALG6)
/* M&S concurrent queue */
PERQ volatile qnode* volatile head;
PERQ volatile qnode* volatile tail;

void q_setup(){
  qnode* node = new qnode;
  node->next = 0;
  head = tail = node;
}
void q_enqueue(qnode* node){
  // node is a newly-allocated node
  node->next = 0;
  // not even slightly ABA-safe, but OK for benchmarks
  while (1){
    volatile qnode* t = tail;
    volatile qnode* next = t->next;
    if (tail == t){
      if (next == 0){
        if (__sync_bool_compare_and_swap(&t->next, next, node)){
          __sync_bool_compare_and_swap(&tail, t, node);
          return;
        }
      }else{
        __sync_bool_compare_and_swap(&tail, t, next);
      }
    }
  }
}
qnode* q_dequeue(){
  while (1){
    volatile qnode* h = head;
    volatile qnode* t = tail;
    volatile qnode* next = h->next;
    if (h == head){
      if (h == t){
        assert(next);
        __sync_bool_compare_and_swap(&tail, t, next);
      }else{
        int value, id;
        value = next->value;
        id = next->id;
        if (__sync_bool_compare_and_swap(&head, h, next)){
          h->value = value;
          h->id = id;
          return (qnode*)h;
        }
      }
    }
  }
}

void q_end(){}
#elif defined(ALG7)
/* M&S two-lock queue */
PERQ volatile qnode* volatile head;
PERQ volatile qnode* volatile tail;
PERQ int* headlock;
PERQ int* taillock;
void q_setup(){
  headlock = new int(0);
  taillock = new int(0);
  qnode* node = new qnode;
  node->next = 0;
  head = tail = node;
}
void q_enqueue(qnode* node){
  node->next = 0;
  spin_lock(taillock);
  tail->next = node;
  tail = node;
  spin_unlock(taillock);
}
qnode* q_dequeue(){
  spin_lock(headlock);
  qnode* node = (qnode*)head;
  volatile qnode* newhead = node->next;
  assert(newhead);
  int value = newhead->value;
  int id = newhead->id;
  head = newhead;
  spin_unlock(headlock);
  node->value = value;
  node->id = id;
  return (qnode*)node;
}
void q_end(){}
#endif







struct processor_info{
  int count;
  int id;
  int nproc;
};

pthread_barrier_t barrier;
volatile int nrunning;

void* processor(void* p_){
#ifdef PQ
  q_setup();
#endif
  processor_info* p = (processor_info*)p_;
  int* counts = new int[p->nproc];
  int* max = new int[p->nproc];
  qnode** last = new qnode*[p->nproc];
  for (int i=0;i<p->nproc;i++) counts[i]=0, max[i]=-1, last[i]=0;
  qnode* nodes = new qnode[p->count];
  for (int i=0;i<p->count;i++) nodes[i].next = 0;

  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(p->id, &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
  for (int j=0; j<10; j++){
    for (int i=0; i<1000; i++) (void)nrunning;
    sched_yield();
  }
  __sync_fetch_and_add(&nrunning, 1);
  pthread_barrier_wait(&barrier);
  for (int i=0; i<p->count; ){
    int n = min(p->count - i, 3);
    for (int j=0; j<n; j++){
      nodes[i+j].value = i+j;
      nodes[i+j].id = p->id;
#ifdef DBG
      printf("producer %d sending %d\n", p->id, nodes[i+j].value);
#endif
      q_enqueue(&nodes[i+j]);
    }
    for (int j=0; j<n; j++){
      qnode* q = 0;
      while (!q){
        q = q_dequeue();
#ifdef DBG
        if (q) printf("consumer %d got %d from %d\n", p->id, q->value, q->id);
        else printf("consumer %d EMPTY\n", p->id);
#endif
      }
      assert(q != last[q->id]);
      assert(q->value != max[q->id]);
      assert(q->value > max[q->id]);
      max[q->id] = q->value;
      last[q->id] = q;
      counts[q->id]++;
    }
    i+=n;
  }

  q_end();
  return counts;
}

struct timespec curtime(clockid_t clk){
  struct timespec t;
  clock_gettime(clk, &t);
  return t;
}
double since(clockid_t clk, struct timespec then){
  struct timespec now = curtime(clk);
  return (double)(now.tv_sec - then.tv_sec) + 
    ((double)(now.tv_nsec - then.tv_nsec)) * (1.0 / 1e9);
}

int main(){
#ifndef PQ
  q_setup();
#endif
  pthread_barrier_init(&barrier, 0, NPROC + 1);
  pthread_t* processors = new pthread_t[NPROC];
  processor_info** pinfos = new processor_info*[NPROC];
  int total = TOKEN;
  for (int i=0; i<NPROC; i++){
    int n = min(total, TOKEN / NPROC + 1);
    processor_info* p = new processor_info;
    pinfos[i] = p;
    p->count = n;
    p->id = i;
    p->nproc = NPROC;
    pthread_create(&processors[i], 0, processor, p);
    total -= n;
  }
  int** countrets = new int*[NPROC];

  while (nrunning < NPROC);
  clockid_t clk = CLOCK_MONOTONIC;
  struct timespec start = curtime(clk);
  pthread_barrier_wait(&barrier);
  
  for (int i=0; i<NPROC; i++) pthread_join(processors[i], (void**)&countrets[i]);
  printf("%.6f\n", since(clk, start));

  int* totalsent = new int[NPROC];
  for (int i=0; i<NPROC; i++) totalsent[i] = 0;
  for (int i=0; i<NPROC; i++){
    printf("Producer %d sent %d\n", i, pinfos[i]->count);
    for (int j=0; j<NPROC; j++){
      printf("  Consumer %d got %d from producer %d\n", i, countrets[i][j], j);
      totalsent[j] += countrets[i][j];
    }
  }
  for (int i=0; i<NPROC; i++) assert(totalsent[i] == pinfos[i]->count);
  return 0;
}
