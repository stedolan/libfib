#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <new>

#include OVERRIDE_PTHREAD_H


#define N 10
struct buffer{
  int items[N];
  int start, len;
  pthread_cond_t wait_empty;
  pthread_cond_t wait_full;
  pthread_mutex_t mutex;
  buffer(){
    start = len = 0;
    pthread_mutex_init(&mutex, 0);
    pthread_cond_init(&wait_empty, 0);
    pthread_cond_init(&wait_full, 0);
  }
  void add(int data){
    pthread_mutex_lock(&mutex);
    while (len == N){
      pthread_cond_wait(&wait_full, &mutex);
    }
    assert(len < N);
    items[(start + len)%N] = data;
    len++;
    if (len == 1){
      pthread_cond_broadcast(&wait_empty);
    }
    pthread_mutex_unlock(&mutex);
  }

  int remove(){
    pthread_mutex_lock(&mutex);
    while (len == 0){
      pthread_cond_wait(&wait_empty, &mutex);
    }
    assert(len > 0);
    int data = items[start];
    len--;
    start = (start + 1) % N;
    if (len == N-1){
      pthread_cond_broadcast(&wait_full);
    }
    pthread_mutex_unlock(&mutex);
    return data;
  }
};
#ifndef COUNT
#define COUNT 200000
#endif
void* producer(void* b){
  buffer* buf = (buffer*)b;
  for (int i=0;i<COUNT;i++){
    buf->add(i);
#ifndef QUIET
    printf("P%d\n",i);
#endif
  }
}
void* consumer(void* b){
  buffer* buf = (buffer*)b;
  for (int i=0;i<COUNT*2;i++){
    int x = buf->remove();
#ifndef QUIET
    printf("C%d\n",x);
#endif
  }
}

int main(){
  pthread_t threads[3 * 4];
  for (int i=0; i<12; i+=3){
    buffer* buf = new (malloc(256)) buffer();
    pthread_create(&threads[i+0], 0, producer, buf);
    pthread_create(&threads[i+1], 0, producer, buf);
    pthread_create(&threads[i+2], 0, consumer, buf);
  }
  for (int i=0;i<12;i++) pthread_join(threads[i], 0);
}
