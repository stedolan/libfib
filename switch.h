#ifndef SWITCH_H
#define SWITCH_H

#include "llq.h"

typedef void (func_t)(void);


static inline void announce_paused(func_t** loc, func_t* f){
  // FIXME barriers?
  *loc = f;
}

template <class Ret, class Arg>
struct switcher{
  struct retval{
    func_t* function;
    func_t** store_loc;
    Ret userdata;
  };
  typedef __attribute__((swapstack)) retval (*t)(func_t**,Arg);

  static inline Ret apply(t target, func_t** loc, Arg value){
    retval ret = target(loc, value);
    announce_paused(ret.store_loc, ret.function);
    return ret.userdata;
  }
};

template <class Ret>
struct switcher<Ret, void>{
  struct retval{
    func_t* function;
    func_t** store_loc;
    Ret userdata;
  };
  typedef __attribute__((swapstack)) retval (*t)(func_t**);

  static inline Ret apply(t target, func_t** loc){
    retval ret = target(loc);
    announce_paused(ret.store_loc, ret.function);
    return ret.userdata;
  }
};


template <class Arg>
struct switcher<void, Arg>{
  struct retval{
    func_t* function;
    func_t** store_loc;
  };
  typedef __attribute__((swapstack)) retval (*t)(func_t**,Arg);

  static inline void apply(t target, func_t** loc, Arg value){
    retval ret = target(loc, value);
    announce_paused(ret.store_loc, ret.function);
  }
};

template <>
struct switcher<void, void>{
  struct retval{
    func_t* function;
    func_t** store_loc;
  };
  typedef __attribute__((swapstack)) retval (*t)(func_t**);
  static inline void apply(t target, func_t** loc){
    retval ret = target(loc);
    announce_paused(ret.store_loc, ret.function);
  }
};



template <class Ret>
struct waiter : public llq::node{
  func_t* func;
  waiter() : func(0) {}
  func_t* read_func(){
    return (func_t*)llq::await_initialisation((llq::word*)&func);
  }

  template <class Arg>
  inline Ret invoke(waiter<Arg>* other, Arg arg){
    typedef typename switcher<Ret,Arg>::t switch_t;
    switch_t other_f = (switch_t)other->read_func();
    return switcher<Ret,Arg>::apply(other_f, &func, arg);
  }

  inline Ret invoke(waiter<void>* other){
    typedef typename switcher<Ret,void>::t switch_t;
    switch_t other_f = (switch_t)other->read_func();
    return switcher<Ret,void>::apply(other_f, &func);
  }
};


#endif
