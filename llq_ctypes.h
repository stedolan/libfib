#ifndef LLQ_CTYPES_H
#define LLQ_CTYPES_H


/* This file contains the basic type definitions for llq, so
   that they may be included in structure definitions exposed
   to C code (i.e. the pthreads compatibility layer). */

#if defined(__x86_64__)
#define LLQ_X86_64 1
#define LLQ_HAS_ATOMIC_XCHG 1
#define LLQ_HAS_DCAS 1
#define LLQ_HAS_FETCH_ADD 1
#elif defined(__PPC__) && defined(__PPC64__)
#define LLQ_PPC64 1
#define LLQ_HAS_LL_SC 1
#else
#error "unknown platform"
#endif


/* A word is an unsigned integer the same size as a pointer */
#if defined(LLQ_X86_64)
typedef __attribute__((may_alias)) unsigned long llq_word;
#elif defined(LLQ_PPC64)
typedef __attribute__((may_alias)) unsigned long llq_word;
#endif

#define LLQ_STATIC_ASSERT(expr) \
  int __static_assert(int static_assert_failed[(expr)?1:-1]);

/* llq_word is the same size as a pointer */
LLQ_STATIC_ASSERT(sizeof(llq_word) == sizeof(void*))

/* llq_word is unisigned */
LLQ_STATIC_ASSERT(((llq_word)-1) > 0)


/* A linked list node. Generally followed by other data. */
struct llq_node{
  struct llq_node* next;
};


#ifdef LLQ_HAS_DCAS
/* ABA-safe counted pointers */
struct __attribute__((aligned(16))) llq_ctptr{
  llq_word ptr;
  llq_word serial;
};
#elif LLQ_HAS_LL_SC
struct llq_ctptr{
  llq_word ptr;
};
#endif

/* Queue head structure */
struct llq_queue{
  struct llq_node head;
  struct llq_ctptr tail;
};



#endif
