

#include <stdio.h>
#include <vector>
#include <cassert>
#include <cstdlib>
#include "sync_object.h"
#include <sched.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "io.h"

#include "spawn.h"



#define N 10
struct buffer{
  int items[N];
  int start, len;
  condition_var wait_empty;
  condition_var wait_full;
  mutex mtx;
  buffer(){
    start = len = 0;
  }
  void add(int data){
    mtx.lock();
    while (len == N){
      wait_full.wait(&mtx);
    }
    assert(len < N);
    items[(start + len)%N] = data;
    len++;
    if (len == 1){
      wait_empty.broadcast();
    }
    mtx.unlock();
  }

  int remove(){
    mtx.lock();
    while (len == 0){
      wait_empty.wait(&mtx);
    }
    assert(len > 0);
    int data = items[start];
    len--;
    start = (start + 1) % N;
    if (len == N-1){
      wait_full.broadcast();
    }
    mtx.unlock();
    return data;
  }
};
#undef N
buffer* buf;
// FIXME starvation
void bufproducer(int){
  for (int i=0;i<1000000;i++){
    buf->add(i);
    //    printf("P%d\n", i);
  }
}
void bufconsumer(int){
  for (int i=0;i<2000000;i++){
    int x = buf->remove();
    //    printf("C%d\n", x);
  }
}

void bufmain(){
  buf = new buffer;
  worker::current().new_fiber(bufproducer,0);
  worker::current().new_fiber(bufproducer,0);
  worker::current().new_fiber(bufconsumer,0);
}



















mutex mtx;
condition_var cond;

void yieldmany(int id, int ycount){
  for (int i=0;i<ycount;i++){
    say("<MTX%d> yielding\n", id);
    worker::current().yield();
    sched_yield();
  }
}

void mutexfunc(int id){
  int ycount1[] = {5,3};
  int ycount2[] = {10,10};
  yieldmany(id, ycount1[id]);
  say("<MTX%d> wants lock\n", id);
  mtx.lock();
  say("<MTX%d> has lock\n", id);
  yieldmany(id, ycount2[id]/2);
  if (id == 0){
    say("<MTX%d> waiting\n", id);
    cond.wait(&mtx);
    say("<MTX%d> woke\n", id);
  }else{
    say("<MTX%d> signaling\n", id);
    cond.signal();
  }
  yieldmany(id, ycount2[id]/2);
  say("<MTX%d> releasing lock\n", id);
  mtx.unlock();
  say("<MTX%d> released lock\n", id);
  yieldmany(id, ycount1[id]);
}

void migrator(int){
  for (int i=0;i<20;i++){
    int dest = i % worker::nworkers;
    say("Migrating to %d\n", dest);
    worker::current().migrate(dest);
  }
  say("Finished Migrating\n");
}
#ifdef QUIET
#define NRING 1000
#define NTOKEN 100
#define NCHANTOKEN 10000
#define NQ 100
#else
#define NRING 50
#define NTOKEN 10
#define NCHANTOKEN 2
#define NQ 10
#endif


blocking_channel<int>* q[NQ];
void queue_writer(int id){
  blocking_channel<int>* myq = q[(id/64)%NQ];
  for (int i=0; i<100; i++){
    say("[W%d] writing %d\n", id, i);
    myq->write(i);
  }
}

void queue_reader(int id){
  blocking_channel<int>* myq = q[(id/64)%NQ];
  say("[R%d] starting\n", id);
  while (1){
    int x = myq->read();
    say("[R%d] read %d\n", id, x);
  }
}


blocker<int> b;
void joiner(int id){
  if (id == 0){
    say("$B%d: accepting\n", id);
    int got = b.accept()->data;
    say("$B%d: Got %d\n", id, got);
  }else{
    say("$B%d: blocking\n", id);
    b.block(42);
    say("$B%d: unblocked\n", id);
  }
}


blocking_channel<int> ring[100];
void ringer(int id){
  while (1){
    int x = ring[id].read();
    say("%d --> %d [%d]\n", id, id+1, x);
    ring[id+1].write(x);
  }
}
void mainringer(){
  for (int i=0; i<NRING-1;i++){
    worker::current().new_fiber(ringer, i);
  }
  int tokensent = 0;
  int tokenreceived = 0;
  for (int i=0; i<NTOKEN; i++){
    for (int j=0; j<NRING/2; j++){
      ring[0].write(tokensent);
      tokensent++;
    }
    for (int j=0; j<NRING/2; j++){
      int t =ring[NRING-1].read();
      int r = tokenreceived++;
      assert(t == r);
    }
  }
}

int thunk(void*){
  say("thunk running\n");
  return 42;
}

int spawner(void*){
  say("spawner running\n");
  fiber_handle<int> s1 = spawn_fiber(thunk, (void*)0);
  say("spawner waiting\n");
  printf("blocker %p\n", (void*)s1.fib);
  s1.join();
  say("spawner returning\n");
  return 42;
}


void testread(int fd){
  char buf;
  say("Read: from fd %d\n", fd);
  ssize_t r = worker::current().iomgr.read(fd, &buf, 1);
  say("Read: %c: [%d %s]\n", buf, (int)r, r < 0 ? strerror(-r) : "OK");
}

void testwrite(int fd){
  char buf = 'X';
  say("Sleeping\n");
  worker::current().iomgr.sleep_for(timestamp::SECONDS(3));
  say("Write: to fd %d\n", fd);
  ssize_t r = worker::current().iomgr.write(fd, &buf, 1);
  say("Write: %c: [%d %s]\n", buf, (int)r, r < 0 ? strerror(-r) : "OK");
}
void sleeploop(int){
  while (1){
    worker::current().iomgr.sleep_for(timestamp::MILLISECONDS(6000));
  }
}

int syscheck(int ret){
  if (ret < 0){
    perror(0);
  }
  return ret;
}


void rdbench(int fd){
  for (int i=0; i<10000; i++){
    char buf[1000];
    worker::current().iomgr.read(fd, buf, 1000);
  }
}
void wrbench(int fd){
  for (int i=0; i<10000; i++){
    char buf[1000] = "X";
    worker::current().iomgr.write(fd, buf, 1000);
  }
}
void iobench1(int){
  int fds[2];
  pipe(fds);
  worker::current().iomgr.setup_fd(fds[0]);
  worker::current().iomgr.setup_fd(fds[1]);
  worker::current().new_fiber(rdbench, fds[0]);
  worker::current().new_fiber(wrbench, fds[1]);
}
void startiobench(int){
  for (int i=0; i<4;i++){
    iobench1(0);
  }
}


void client(int){
  int sock = syscheck(socket(AF_INET, SOCK_STREAM, 0));
  worker::current().iomgr.setup_fd(sock);
  struct sockaddr_in addr = {AF_INET, htons(22)};
  inet_aton("134.226.83.48", &addr.sin_addr);
  syscheck(worker::current().iomgr.connect(sock, (const struct sockaddr*)&addr, sizeof(addr)));
  char buf[100];
  int r = syscheck(worker::current().iomgr.read(sock, buf, 100));
  ::write(1, buf, r);
}

void client_handler(int fd){
  for (int i=0; i<100; i++){
    worker::current().iomgr.write(fd, "hello\n", 6);
    worker::current().iomgr.sleep_for(timestamp::MILLISECONDS(250));
  }
  close(fd);
}

void server(int){
  int servsock = syscheck(socket(AF_INET, SOCK_STREAM, 0));
  struct sockaddr_in addr = {AF_INET, htons(2000), 0};
  int reuseaddr = 1;
  syscheck(setsockopt(servsock, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr)));
  syscheck(bind(servsock, (const struct sockaddr*)&addr, sizeof(addr)));
  syscheck(listen(servsock, 5));

  say("Server socket listening\n");
  worker::current().iomgr.setup_fd(servsock);

  while (1){
    struct sockaddr_in clientaddr;
    socklen_t clientaddr_len = sizeof(clientaddr);
    say("waiting for connection\n");
    int client = syscheck(worker::current().iomgr.accept(servsock, (struct sockaddr*)&clientaddr, &clientaddr_len));
    say("Got connection: %d\n", client);
    worker::current().new_fiber(client_handler, client);
  }

  close(servsock);
}

void testio(int){
  /*
  int pipefd[2];
  pipe(pipefd);
  worker::current().iomgr.setup_fd(pipefd[0]);
  worker::current().iomgr.setup_fd(pipefd[1]);
  worker::current().new_fiber(testread, pipefd[0]);
  worker::current().new_fiber(testwrite, pipefd[1]);
  
  worker::current().new_fiber(sleeploop, 0);
  
  worker::current().new_fiber(server, 0);
  worker::current().new_fiber(client, 0);
  */
  startiobench(0);
}


void main_fiber(int){
  //  bufmain();
  //  return;
  //  for (int i=0; i<NQ; i++) q[i] = llq::cachealign_new< blocking_channel<int> >();
  //   mainringer();
  

  //  worker::current().new_fiber(migrator, 0);
  
  
  
  say("Main creating MTX0\n");
  worker::current().new_fiber(mutexfunc,0);
  say("Main creating MTX1\n");
  worker::current().new_fiber(mutexfunc,1);
  

  worker::current().new_fiber(testio,0);
  
  
  //  fiber_new(fiber_a,0);
  //  fiber_new(fiber_b,0);
  //  fiber_new(fiber_c,0);
  /*  spawn_fiber(spawner, (void*)0);
  worker::current().new_fiber(joiner, 0);
  worker::current().new_fiber(joiner, 1);*/

  

  /*
  for (int i=0;i<NCHANTOKEN;i++){

    worker::current().new_fiber(queue_writer,i);
    worker::current().new_fiber(queue_reader,i);
  }
  */
  
  

  //fiber_new(requestor, 0);
  //fiber_new(responder,0);
  //fiber_new(proxy, 0);
  //fiber_new(requestor,1);
  say("Ending main fiber\n");
}

void pause_on_err(int){
  write(2, "died\n", 5);
  pause();
}


int main(int argc, char** argv){
  setvbuf(stdout, 0, _IONBF, 0);
  signal(SIGSEGV, pause_on_err);
  signal(SIGINT, pause_on_err);
  worker::spawn_workers(atoi(argv[1]));
  main_fiber(0);
  //worker::spawn_and_join_workers(atoi(argv[1]), main_fiber);
  worker::await_completion();
  return 0;
}
