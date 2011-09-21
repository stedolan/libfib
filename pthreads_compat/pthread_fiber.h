#ifdef __cplusplus
#define CLINKAGE extern "C"
#else
#define CLINKAGE
#endif

typedef struct pthread_mutex_t pthread_mutex_t;

typedef struct pthread_mutexattr_t pthread_mutexattr_t;

CLINKAGE int pthread_mutex_init(pthread_mutex_t* mutex, pthread_mutexattr_t* attr);
CLINKAGE int pthread_mutex_lock(pthread_mutex_t* mutex);
CLINKAGE int pthread_mutex_unlock(pthread_mutex_t* mutex);
CLINKAGE int pthread_mutex_destroy(pthread_mutex_t* mutex);

CLINKAGE int pthread_create(pthread_t* thread, pthread_attr_t* attr,
                            void* (*start_fn)(void*), void* arg);
CLINKAGE int pthread_detach(pthread_t thread);
CLINKAGE int pthread_join(pthread_t thread, void** retptr);

