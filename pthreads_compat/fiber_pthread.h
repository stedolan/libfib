#ifndef FIBER_PTHREAD_H
#define FIBER_PTHREAD_H
#ifdef __cplusplus
#define CLINKAGE extern "C"
#else
#define CLINKAGE
#endif

#include "../llq_ctypes.h"
#ifndef FIBER_FULL_DEFINITIONS
#define FIBER_TYPES_ONLY
#endif
#include "../sync_object.h"

#ifndef FIBER_PTHREAD_NO_RENAME
#define pthread_mutex_t fiber_pthread_mutex_t
#define pthread_mutexattr_t fiber_pthread_mutexattr_t
#define pthread_mutex_init fiber_pthread_mutex_init
#define pthread_mutex_lock fiber_pthread_mutex_lock
#define pthread_mutex_unlock fiber_pthread_mutex_unlock
#define pthread_mutex_destroy fiber_pthread_mutex_destroy
#define pthread_cond_t fiber_pthread_cond_t
#define pthread_condattr_t fiber_pthread_condattr_t
#define pthread_cond_init fiber_pthread_cond_init
#define pthread_cond_wait fiber_pthread_cond_wait
#define pthread_cond_signal fiber_pthread_cond_signal
#define pthread_cond_broadcast fiber_pthread_cond_broadcast
#define pthread_cond_timedwait UNIMPLEMENTED
#define pthread_cond_destroy fiber_pthread_cond_destroy
#define pthread_create fiber_pthread_create
#define pthread_detach fiber_pthread_detach
#define pthread_join fiber_pthread_join
#define pthread_t fiber_pthread_t
#define pthread_self UNIMPLEMENTED
#define pthread_equal UNIMPLEMENTED
#define pthread_attr_t fiber_pthread_attr_t
#define pthread_attr_init fiber_pthread_attr_init
#define pthread_attr_setscope fiber_pthread_attr_setscope
#define pthread_attr_setstacksize fiber_pthread_attr_setstacksize
#define pthread_yield fiber_pthread_yield
#endif

typedef struct fiber_pthread_mutex_t {
  struct fiber_sync_object sync;
} fiber_pthread_mutex_t;

typedef struct fiber_pthread_cond_t {
  struct fiber_sync_object sync;
} fiber_pthread_cond_t;

typedef struct fiber_pthread_mutexattr_t fiber_pthread_mutexattr_t;


typedef void* fiber_pthread_t;
typedef struct {
  size_t stacksize;
} fiber_pthread_attr_t;
typedef int fiber_pthread_condattr_t;

CLINKAGE int fiber_pthread_mutex_init(fiber_pthread_mutex_t* mutex, fiber_pthread_mutexattr_t* attr);
CLINKAGE int fiber_pthread_mutex_lock(fiber_pthread_mutex_t* mutex);
CLINKAGE int fiber_pthread_mutex_unlock(fiber_pthread_mutex_t* mutex);
CLINKAGE int fiber_pthread_mutex_destroy(fiber_pthread_mutex_t* mutex);

CLINKAGE int fiber_pthread_cond_init(fiber_pthread_cond_t* cond, fiber_pthread_condattr_t* attr);
CLINKAGE int fiber_pthread_cond_wait(fiber_pthread_cond_t* cond, fiber_pthread_mutex_t* mtx);
CLINKAGE int fiber_pthread_cond_signal(fiber_pthread_cond_t* cond);
CLINKAGE int fiber_pthread_cond_broadcast(fiber_pthread_cond_t* cond);
CLINKAGE int fiber_pthread_cond_destroy(fiber_pthread_cond_t* cond);


CLINKAGE int fiber_pthread_attr_init(fiber_pthread_attr_t* attr);
CLINKAGE int fiber_pthread_attr_setscope(fiber_pthread_attr_t* attr, int scope);
CLINKAGE int fiber_pthread_attr_setstacksize(fiber_pthread_attr_t* attr, size_t stacksize);
CLINKAGE int fiber_pthread_create(fiber_pthread_t* thread, fiber_pthread_attr_t* attr,
                            void* (*start_fn)(void*), void* arg);
CLINKAGE int fiber_pthread_detach(fiber_pthread_t thread);
CLINKAGE int fiber_pthread_join(fiber_pthread_t thread, void** retptr);
CLINKAGE int fiber_pthread_yield();
#endif
