#include <vector>
#include <stdint.h>
#include <queue>
#include <functional>
#include <utility>
#include <unistd.h>
#include <sys/socket.h>
#ifndef IO_H
#define IO_H


namespace timestamp{
  // Nanoseconds since Jan 1. 1970 (scaled Unix time)
  typedef int64_t timestamp;

  // Time difference in nanoseconds
  typedef int64_t timediff;
  
  enum {
    T_NANOSECONDS = 1,
    T_MICROSECONDS = T_NANOSECONDS * 1000,
    T_MILLISECONDS = T_MICROSECONDS * 1000,
    T_SECONDS = T_MILLISECONDS * 1000,
    T_MINUTES = T_SECONDS * 60,
    T_HOURS = T_MINUTES * 60
  };
  inline timestamp NANOSECONDS(int n){
    timestamp t = T_NANOSECONDS;
    t *= n;
    return t;
  }
  inline timestamp MICROSECONDS(int n){
    timestamp t = T_MICROSECONDS;
    t *= n;
    return t;
  }
  inline timestamp MILLISECONDS(int n){
    timestamp t = T_MILLISECONDS;
    t *= n;
    return t;
  }
  inline timestamp SECONDS(int n){
    timestamp t = T_SECONDS;
    t *= n;
    return t;
  }
  
  // Current time
  timestamp now();
}

struct fd_handler;
struct selector;
struct worker;

enum opcode { OP_READ = 1, OP_WRITE = 2, OP_ACCEPT = 4, OP_CONNECT = 8 };
// There is one io_manager per worker
struct io_manager{


  std::vector<fd_handler*> handlers;
  typedef std::pair<timestamp::timestamp, waiter<void>*> timer;
  std::priority_queue<timer, std::vector<timer>, std::greater<timer> > timers;
  selector* sel;
  worker* current_worker;

  io_manager(worker*);
  ~io_manager();

  fd_handler* get_handler(int fd);

  void setup_fd(int fd);
  
  void sleep_for(timestamp::timediff duration);
  void sleep_until(timestamp::timestamp when);

  int wake_all_until(llq::queue* wokenq, timestamp::timestamp when);
  int perform_io(llq::queue* wokenq);
  bool has_outstanding_io();

  int connect(int fd, const struct sockaddr* addr, socklen_t addrlen);
  int accept(int fd, struct sockaddr* addr, socklen_t* addrlen);
  ssize_t read(int fd, void* buf, size_t count);
  ssize_t write(int fd, const void* buf, size_t count);

  int close(int fd);

  void add_waiter(fd_handler* fd, opcode op);
};
#endif
