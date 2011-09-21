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

