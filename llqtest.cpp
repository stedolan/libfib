#include <pthread.h>
#include <assert.h>
#include <algorithm>
#include <stdio.h>
#include <unistd.h>
#include "llq.h"
using namespace std;
using namespace llq;

struct qnode : public node{
  int value, id;
};

queue thequeue;

void q_setup(){ 
  queue_init(&thequeue, 1);
}
void q_enqueue(qnode* node){
  typedef concurrent_ops ops;
  while (1){
    word tag = ops::get_state(&thequeue);
    if (ops::isempty(&thequeue, tag)){
      assert(ops::tag(&thequeue, tag) == 1);
    }else{
      assert(ops::tag(&thequeue, tag) == 0);
    }
    if (ops::try_enqueue(&thequeue, node, tag, 0)){
      return;
    }
  }
}
qnode* q_dequeue(){
  typedef concurrent_ops ops;
  while (1){
    word tag = ops::get_state(&thequeue);
    node* ret;
    if (ops::isempty(&thequeue, tag)){
      assert(ops::tag(&thequeue, tag) == 1);
      return 0;
    }else{
      assert(ops::tag(&thequeue, tag) == 0);
      if (ops::try_dequeue(&thequeue, &ret, tag, 1)){
        return (qnode*)ret;
      }
    }
  }
}
void q_end(){}




struct processor_info{
  int count;
  int id;
  int nproc;
};

pthread_barrier_t barrier;
volatile int nrunning;

void* processor(void* p_){
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


#if 0
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
#else
  int recv = 0, sent = 0;
  while (recv < p->count || sent < p->count){
    qnode* q = 0;
    bool progress = false;
    int nfails = 20;
    while (recv < p->count && nfails > 0){
      q = q_dequeue();
      if (q){
#ifdef DBG
        printf("consumer %d got %d from %d\n", p->id, q->value, q->id);
#endif
        assert(q != last[q->id]);
        assert(q->value != max[q->id]);
        assert(q->value > max[q->id]);
        max[q->id] = q->value;
        last[q->id] = q;
        counts[q->id]++;
        recv++;
        progress = true;
      }else{
#ifdef DBG
        printf("consumer %d EMPTY\n", p->id);
#endif
        nfails--;
      }
    }
    while ((!progress && sent < p->count) || sent < recv){
      qnode* n = &nodes[sent];
      n->value = sent;
      n->id = p->id;
#ifdef DBG
      printf("producer %d sending %d\n", p->id, n->value);
#endif
      q_enqueue(n);
      sent++;
      progress = true;
    }
  }
  assert(sent == p->count && recv == p->count);
#endif



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
  q_setup();
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
